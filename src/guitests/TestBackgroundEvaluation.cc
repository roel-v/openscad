#include "TestBackgroundEvaluation.h"
#include "gui/evaluation/EvaluationService.h"
#include "gui/evaluation/EvaluationPipeline.h"
#include "gui/evaluation/EvaluationController.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/QGLView.h"
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <thread>

using namespace BackgroundEvaluation;
namespace {
struct Release {
  std::shared_ptr<std::atomic_bool> value = std::make_shared<std::atomic_bool>(false);
  ~Release() { value->store(true); }
};
struct CancelOnExit {
  Job::Ptr job;
  ~CancelOnExit() { job->cancellation->store(true); Service::instance().cancel(job->key); }
};
Job::Ptr blockingJob(const QString& key, const Release& release,
                     const std::shared_ptr<std::atomic_bool>& started)
{
  auto job = std::make_shared<Job>();
  job->key = key;
  job->compute = [started, released = release.value] {
    started->store(true);
    while (!released->load()) {
      evaluationCheckpoint();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  return job;
}
}

void TestBackgroundEvaluation::explicitPriorityAndCoalescing()
{
  auto& service = Service::instance();
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  Release release;
  auto started = std::make_shared<std::atomic_bool>(false);
  auto blocker = blockingJob("test:block", release, started);
  CancelOnExit stop{blocker};
  QVERIFY(service.submit(blocker));
  QTRY_VERIFY(started->load());
  auto order = std::make_shared<QStringList>();
  auto make = [&](QString key, QString name, bool automatic) {
    auto job = std::make_shared<Job>();
    job->key = key; job->automatic = automatic;
    job->compute = [] {};
    std::weak_ptr<Job> weak = job;
    job->complete = [weak, order, name] {
      auto job = weak.lock();
      if (job && job->succeeded && !job->cancellation->load()) order->append(name);
    };
    return job;
  };
  auto obsolete = make("test:auto", "obsolete", true);
  QVERIFY(service.submit(obsolete));
  QVERIFY(service.submit(make("test:explicit", "explicit", false)));
  QVERIFY(!service.submit(make("test:explicit", "timer", true)));
  QVERIFY(service.submit(make("test:auto", "latest", true)));
  QVERIFY(obsolete->cancellation->load());
  release.value->store(true);
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  QCOMPARE(*order, (QStringList{"explicit", "latest"}));
}

void TestBackgroundEvaluation::cancellationKeepsEventLoopResponsive()
{
  auto& service = Service::instance();
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  Release release;
  auto started = std::make_shared<std::atomic_bool>(false);
  auto job = blockingJob("test:cancel", release, started);
  CancelOnExit stop{job};
  auto beats = std::make_shared<int>(0);
  QTimer heartbeat;
  connect(&heartbeat, &QTimer::timeout, &heartbeat, [beats] { ++*beats; });
  heartbeat.start(10);
  QVERIFY(service.submit(job));
  QTRY_VERIFY(started->load());
  QTRY_VERIFY(*beats >= 3);
  service.cancel(job->key);
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  QVERIFY(job->cancellation->load());
  QVERIFY(!job->succeeded);
  auto next = std::make_shared<Job>();
  next->key = "test:recovery"; next->compute = [] { evaluationCheckpoint(); };
  QVERIFY(service.submit(next));
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  QVERIFY(next->succeeded);
}

void TestBackgroundEvaluation::cancelLanguageThenEvaluateAgain()
{
  auto& service = Service::instance();
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  auto run = [&](const std::string& text) {
    auto request = std::make_shared<Request>();
    request->root = {"session:test-evaluation", text, {}, "test-evaluation", 1};
    request->mode = Mode::Preview;
    request->variables.preview = true; request->variables.time = 0;
    auto result = std::make_shared<Result>();
    auto job = std::make_shared<Job>();
    job->key = "test:language";
    std::weak_ptr<Job> weak = job;
    job->compute = [request, result, weak] { if (auto job = weak.lock()) evaluate(*request, *result, *job); };
    service.submit(job);
    return std::make_pair(job, result);
  };
  auto [job, discarded] = run("function spin(n) = spin(n+1); cube(spin(0));");
  CancelOnExit stop{job};
  QTRY_VERIFY_WITH_TIMEOUT(job->currentStage() == "Evaluating parameters" ||
                          job->currentStage() == "Evaluating language", 10000);
  service.cancel(job->key);
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  QVERIFY(!job->succeeded);
  auto [next, result] = run("cube(2);");
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  QVERIFY2(next->succeeded, qPrintable(next->error));
  QVERIFY(result->success);
  QVERIFY(result->products != nullptr);
}

void TestBackgroundEvaluation::renderBothBackends()
{
  auto& service = Service::instance();
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  for (auto backend : {RenderBackend3D::ManifoldBackend, RenderBackend3D::CGALBackend}) {
    auto request = std::make_shared<Request>();
    request->root = {"session:test-render", "difference(){cube(3);cube(2);}", {}, "test-render", 1};
    request->mode = Mode::Render;
    request->variables.preview = false;
    request->settings.backend3D = backend;
    request->colors = ColorMap::instance().defaultColorScheme();
    auto result = std::make_shared<Result>();
    auto job = std::make_shared<Job>();
    job->key = "test:render";
    std::weak_ptr<Job> weak = job;
    job->compute = [request, result, weak] { if (auto job = weak.lock()) evaluate(*request, *result, *job); };
    CancelOnExit stop{job};
    QVERIFY(service.submit(job));
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 30000);
    QVERIFY2(job->succeeded, qPrintable(job->error));
    QVERIFY(result->success);
    QVERIFY(result->geometry != nullptr);
    QVERIFY(!result->geometry->isEmpty());
  }
}

void TestBackgroundEvaluation::capturedParametersRejectMissingOrIncompatibleValues()
{
  auto& service = Service::instance();
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
         {"removed", "2"}, {"size", "invalid"}, {"size", "3"}}) {
    auto request = std::make_shared<Request>();
    request->root = {"session:test-captured", "size = 2;\ncube(size);", {}, "test-captured", 1};
    request->mode = Mode::Preview;
    request->variables.preview = true;
    request->strictParameters = true;
    request->parameters[name].put_value(value);
    auto result = std::make_shared<Result>();
    auto job = std::make_shared<Job>();
    job->key = "test:captured-parameters";
    std::weak_ptr<Job> weak = job;
    job->compute = [request, result, weak] { if (auto job = weak.lock()) evaluate(*request, *result, *job); };
    CancelOnExit stop{job};
    QVERIFY(service.submit(job));
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
    if (value == "3") {
      QVERIFY2(job->succeeded, qPrintable(job->error));
      QVERIFY(result->success);
      QCOMPARE(result->parameters.exportValues("captured").at("size").data(), std::string("3"));
    } else {
      QVERIFY(!job->succeeded);
      QVERIFY(!result->success);
      QVERIFY(job->error.contains("Captured parameter is no longer valid"));
      QVERIFY(!result->scene);
    }
  }
}

void TestBackgroundEvaluation::cancelledQueuedJobNeverComputes()
{
  auto& service = Service::instance();
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  Release release;
  auto started = std::make_shared<std::atomic_bool>(false);
  auto blocker = blockingJob("test:queue-block", release, started);
  CancelOnExit stop{blocker};
  QVERIFY(service.submit(blocker));
  QTRY_VERIFY(started->load());
  auto computed = std::make_shared<std::atomic_bool>(false);
  auto deliveries = std::make_shared<int>(0);
  auto queued = std::make_shared<Job>();
  queued->key = "test:queued-cancel";
  queued->compute = [computed] { computed->store(true); };
  queued->complete = [deliveries] { ++*deliveries; };
  QVERIFY(service.submit(queued));
  service.cancel(queued->key);
  QCOMPARE(*deliveries, 1);
  release.value->store(true);
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  QVERIFY(!computed->load());
  QCOMPARE(*deliveries, 1);
}

void TestBackgroundEvaluation::obsoleteQueuedPreviewRetainsLastDisplay()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = directory.filePath("retained.scad");
  QFile source(path);
  QVERIFY(source.open(QIODevice::WriteOnly));
  source.write("cube(2);\n"); source.close();
  window->designActionAutoReload->setChecked(false);
  window->tabManager->createTab(path);
  auto editor = window->activeEditor;
  QSignalSpy completed(window, &MainWindow::compilationDone);
  window->on_designActionPreview_triggered();
  QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 10000);
  const auto renderer = window->qglview->renderer;
  const auto scene = window->evaluatedScene;
  QVERIFY(renderer != nullptr);
  auto& service = Service::instance();
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  Release release;
  auto started = std::make_shared<std::atomic_bool>(false);
  auto blocker = blockingJob("test:stale-block", release, started);
  CancelOnExit stop{blocker};
  QVERIFY(service.submit(blocker));
  QTRY_VERIFY(started->load());
  completed.clear();
  window->on_designActionPreview_triggered();
  // Queued background work must also block strict annotation/capture actions.
  QVERIFY(window->backgroundEvaluation->busy());
  QVERIFY(window->annotationController->busy());
  QVERIFY(window->annotationController->status().value("busy_reasons").toArray().contains("background_evaluation"));
  editor->setPlainText("cube(3);\n");
  release.value->store(true);
  QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
  QCOMPARE(window->qglview->renderer, renderer);
  QCOMPARE(window->evaluatedScene, scene);
  QVERIFY(completed.isEmpty());
  QVERIFY(!window->evaluationBinding->current());
  editor->setPlainText("cube(2);\n");
  editor->setContentModified(false);
  window->tabManager->closeCurrentTab();
}

void TestBackgroundEvaluation::failedPreviewRetainsLastDisplay()
{
  QTemporaryDir directory;
  const auto path = directory.filePath("failure.scad");
  QFile source(path);
  QVERIFY(source.open(QIODevice::WriteOnly));
  source.write("cube(2);\n"); source.close();
  window->designActionAutoReload->setChecked(false);
  window->tabManager->createTab(path);
  auto editor = window->activeEditor;
  QSignalSpy completed(window, &MainWindow::compilationDone);
  window->on_designActionPreview_triggered();
  QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 10000);
  const auto renderer = window->qglview->renderer;
  const auto scene = window->evaluatedScene;
  QVERIFY(renderer != nullptr);
  completed.clear();
  editor->setPlainText("cube(;\n");
  window->on_designActionPreview_triggered();
  QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 10000);
  QCOMPARE(window->lastCompiledDoc, QString("cube(;\n"));
  QCOMPARE(window->qglview->renderer, renderer);
  QCOMPARE(window->evaluatedScene, scene);
  QVERIFY(!window->evaluationBinding->current());
  editor->setPlainText("cube(4);\n");
  completed.clear();
  window->on_designActionPreview_triggered();
  QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 10000);
  QVERIFY(window->evaluationBinding->current());
  QVERIFY(window->evaluatedScene != scene);
  editor->setContentModified(false);
  window->tabManager->closeCurrentTab();
}
