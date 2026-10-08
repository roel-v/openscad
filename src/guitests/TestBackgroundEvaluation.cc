#include "TestBackgroundEvaluation.h"
#include "gui/evaluation/EvaluationService.h"
#include "gui/evaluation/EvaluationPipeline.h"
#include "gui/evaluation/EvaluationController.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/QGLView.h"
#include "core/CSGNode.h"
#include "core/parsersettings.h"
#include <QEventLoop>
#include <QDir>
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
bool writeLibrary(const QString& path, const QByteArray& bytes)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
std::pair<Job::Ptr, std::shared_ptr<Result>> cachedPreview(Request request, std::vector<std::string> paths = {})
{
  auto result = std::make_shared<Result>();
  auto job = std::make_shared<Job>();
  job->key = QString::fromStdString(request.root.fileKey);
  if (request.validity) job->cancellation = request.validity->cancellation;
  std::weak_ptr<Job> weak = job;
  job->compute = [request = std::move(request), paths = std::move(paths), result, weak] {
    struct RestorePaths {
      std::vector<std::string> previous = get_library_path();
      ~RestorePaths() { set_library_path(std::move(previous)); }
    } restore;
    set_library_path(paths);
    if (auto active = weak.lock()) evaluate(request, *result, *active);
  };
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  QPointer<QEventLoop> waiting(&loop);
  auto completed = std::make_shared<bool>(false);
  job->complete = [waiting, completed] { *completed = true; if (waiting) waiting->quit(); };
  if (Service::instance().submit(job)) {
    timeout.start(15000);
    loop.exec(); // Sleep while the native worker runs; no processEvents polling.
  }
  if (!*completed) { job->cancellation->store(true); return {}; }
  return {job, result};
}
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

void TestBackgroundEvaluation::parsedLibraryReuseTracksBytesAndInspection()
{
  QTRY_VERIFY_WITH_TIMEOUT(!Service::instance().busy(), 10000);
  QTemporaryDir directory; QVERIFY(directory.isValid());
  const auto library = directory.filePath("library.scad");
  QVERIFY(writeLibrary(library, "function size() = 2;\n"));
  Request request;
  request.filename = directory.filePath("root.scad").toStdString();
  request.root = {request.filename, "use <library.scad>\ncube(size());", {}, "cache-root", 1};
  request.variables.preview = true;
  auto run = [&](size_t reused, double width) {
    auto [job, result] = cachedPreview(request);
    QVERIFY(job); QVERIFY2(job->succeeded, qPrintable(job->error));
    QVERIFY(result->success); QVERIFY(result->csg);
    QCOMPARE(result->reusedLibraries, reused);
    QCOMPARE(result->csg->getBoundingBox().sizes().x(), width);
  };
  auto [firstJob, retained] = cachedPreview(request);
  QVERIFY(firstJob && firstJob->succeeded && retained->success && retained->csg);
  QCOMPARE(retained->reusedLibraries, size_t(0));
  run(1, 2); if (QTest::currentTestFailed()) return;
  // Equal-size edits must invalidate even if filesystem timestamp granularity
  // would make an mtime/size cache report a hit.
  QVERIFY(writeLibrary(library, "function size() = 3;\n"));
  run(0, 3); if (QTest::currentTestFailed()) return;
  const auto libraryKey = EvaluationInputs::fileKey(library.toStdString());
  request.editors = {{libraryKey, "function size() = 4;\n", {}, "dirty-library", 8}};
  run(0, 4); if (QTest::currentTestFailed()) return;
  run(1, 4); if (QTest::currentTestFailed()) return;
  request.validity = std::make_shared<InputValidity>();
  request.validity->cancellation = std::make_shared<std::atomic_bool>(false);
  request.validity->invalidate("dirty-library");
  auto [cancelled, discarded] = cachedPreview(request);
  QVERIFY(cancelled && !cancelled->succeeded && cancelled->cancellation->load());
  request.validity.reset();
  run(0, 4); if (QTest::currentTestFailed()) return;
  request.prepareInspection = true;
  run(0, 4); if (QTest::currentTestFailed()) return;
  auto [tracedJob, traced] = cachedPreview(request);
  QVERIFY(tracedJob && tracedJob->succeeded && traced->success && traced->scene && traced->scene->inspectionPrepared);
  QCOMPARE(traced->reusedLibraries, size_t(1));
  request.prepareInspection = false;
  run(0, 4); if (QTest::currentTestFailed()) return;
  request.editors.clear();
  run(0, 3); if (QTest::currentTestFailed()) return;
  QCOMPARE(retained->csg->getBoundingBox().sizes().x(), 2.);
  QCOMPARE(retained->backing->inputs->consumed().at(libraryKey).utf8, std::string("function size() = 2;\n"));
  QVERIFY(writeLibrary(library, "function size() = ;\n"));
  auto [failed, broken] = cachedPreview(request);
  QVERIFY(failed); QVERIFY(!failed->succeeded || !broken->success || broken->errors || broken->warnings);
  QVERIFY(writeLibrary(library, "function size() = 3;\n"));
  run(0, 3); if (QTest::currentTestFailed()) return;
  run(1, 3); if (QTest::currentTestFailed()) return;
  // Semantic model declarations require tracing even when Inspector is off.
  request.root.utf8 = "use <library.scad>\n@{type:Part,id:\"piece\"} module piece(){cube(size());}\n@{type:Component,id:\"item\"} piece();";
  run(0, 3); if (QTest::currentTestFailed()) return;
  auto [modelJob, model] = cachedPreview(request);
  QVERIFY(modelJob && modelJob->succeeded && model->success && model->scene);
  QVERIFY(model->scene->inspectionPrepared);
  QCOMPARE(model->reusedLibraries, size_t(1));
}

void TestBackgroundEvaluation::parsedLibraryReuseTracksSearchesAndConfiguration()
{
  QTRY_VERIFY_WITH_TIMEOUT(!Service::instance().busy(), 10000);
  QTemporaryDir directory; QVERIFY(directory.isValid());
  QVERIFY(QDir(directory.path()).mkdir("search"));
  QVERIFY(writeLibrary(directory.filePath("search/library.scad"), "function size() = 2;\n"));
  Request request;
  request.filename = directory.filePath("root.scad").toStdString();
  request.root = {request.filename, "use <library.scad>\ncube(size());", {}, "shadow-root", 1};
  request.variables.preview = true;
  auto run = [&](size_t reused, double width) {
    auto [job, result] = cachedPreview(request, {directory.filePath("search").toStdString()});
    QVERIFY(job); QVERIFY2(job->succeeded, qPrintable(job->error));
    QVERIFY(result->success && result->csg);
    QCOMPARE(result->reusedLibraries, reused);
    QCOMPARE(result->csg->getBoundingBox().sizes().x(), width);
  };
  run(0, 2); if (QTest::currentTestFailed()) return;
  run(1, 2); if (QTest::currentTestFailed()) return;
  // The old library is unchanged, but a new local file shadows its search-path entry.
  QVERIFY(writeLibrary(directory.filePath("library.scad"), "function size() = 5;\n"));
  run(0, 5); if (QTest::currentTestFailed()) return;
  run(1, 5); if (QTest::currentTestFailed()) return;
  request.definitions = "cache_test = 1;";
  run(0, 5); if (QTest::currentTestFailed()) return;
  request.features[&Feature::ExperimentalLazyUnion] = true;
  run(0, 5); if (QTest::currentTestFailed()) return;
  request.root.utf8 += "\n// Root changed";
  run(0, 5); if (QTest::currentTestFailed()) return;
  run(1, 5);
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
