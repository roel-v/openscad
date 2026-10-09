#include "gui/evaluation/EvaluationController.h"
#include "gui/MainWindow.h"
#include "gui/OpenSCADApp.h"
#include "gui/Editor.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/ViewportStatistics.h"
#include "gui/Animate.h"
#include "gui/ViewportControl.h"
#include "gui/ScintillaEditor.h"
#include "gui/Console.h"
#include "gui/Preferences.h"
#include "gui/parameter/ParameterWidget.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/annotations/AnnotationStore.h"
#include "gui/InspectionPresentationController.h"
#include "gui/EvaluationOverrides.h"
#include "Feature.h"
#include "core/Settings.h"
#include "RenderStatistic.h"
#ifdef ENABLE_PYTHON
#include "python/python_public.h"
#endif
#include "core/SourceFile.h"
#include "core/CSGNode.h"
#include "core/RenderVariables.h"
#include "glview/PolySetRenderer.h"
#include "glview/cgal/CGALRenderer.h"
#include "glview/preview/ThrownTogetherRenderer.h"
#ifdef ENABLE_OPENCSG
#include "glview/preview/OpenCSGRenderer.h"
#endif
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <utility>
#include <tuple>
#include <QApplication>
#include <QSoundEffect>
#include <QSignalBlocker>
#include <QElapsedTimer>

extern std::string commandline_commands;
namespace BackgroundEvaluation {
Controller::Controller(MainWindow *window) : QObject(window), window(window)
{
  (void)Service::instance();
  label = new QLabel(window);
  label->setObjectName("evaluationStatus");
  stop = new QPushButton(tr("Cancel"), window);
  stop->setObjectName("cancelEvaluation");
  window->statusBar()->addPermanentWidget(label);
  window->statusBar()->addPermanentWidget(stop);
  connect(stop, &QPushButton::clicked, this, [this] { cancel(this->window->activeEditor); });
  connect(&timer, &QTimer::timeout, this, [this] { poll(); });
  connect(window->qglview, &QGLView::viewportPaintCompleted, this, [this] {
    if (awaitingFrame.isEmpty()) return;
    awaitingFrame["first_frame_ms"] = double(std::chrono::duration_cast<std::chrono::microseconds>(
      Job::Clock::now() - installedAt).count()) / 1000;
    emit framePresented(std::exchange(awaitingFrame, {}));
  });
  timer.setInterval(50);
  connect(&watcher, &QFileSystemWatcher::fileChanged, this, [this] { ++watcherEpoch; });
  auto configurationChanged = [this] {
    cancelledRevisions.clear();
    for (const auto& [key, attempt] : pending) attempt->job->cancellation->store(true);
    this->window->evaluationBinding->invalidate(tr("Evaluation configuration changed"));
  };
  connect(GlobalPreferences::inst(), &Preferences::ExperimentalChanged, this, configurationChanged);
  connect(GlobalPreferences::inst(), &Preferences::renderBackend3DChanged, this, configurationChanged);
  connect(window->tabManager, &TabManager::editorAboutToClose, this, [this](EditorInterface *editor) {
    cancel(editor);
    if (deferred && deferred->owner == editor) deferred.reset();
  });
  connect(window->tabManager, &TabManager::currentEditorChanged, this,
          [this](EditorInterface *editor) { activate(editor); });
  connect(window->tabManager, &TabManager::editorFileOpened, this,
          [this](EditorInterface *editor) { request(editor, Mode::Preview); });
  for (auto editor : window->tabManager->editorList) bind(editor);
  QTimer::singleShot(0, this, [this] {
    // The initial document was opened before this controller existed. Preview it once,
    // independently of disk auto-reload. Upgrade metadata-only work, but do not supersede
    // a Preview or Render already requested while the window was being initialized.
    for (auto editor : this->window->tabManager->editorList) {
      const auto existing = pending.find(editor->annotations->sessionId);
      if (existing != pending.end() && existing->second->request.mode != Mode::Parameters) continue;
      const auto mode = editor == this->window->activeEditor && !editor->toPlainText().isEmpty()
        ? Mode::Preview : Mode::Parameters;
      request(editor, mode, true);
    }
  });
  poll();
}
Controller::~Controller()
{
  // Disconnect deliveries before MainWindow starts destroying its scene/UI.
  for (auto& [key, job] : pending) { job->job->complete = {}; Service::instance().cancel(key); }
}
bool Controller::busy(EditorInterface *editor) const
{
  return editor && (pending.count(editor->annotations->sessionId) || Service::instance().runningFor(editor->annotations->sessionId));
}
bool Controller::hasDeferred(EditorInterface *editor) const { return deferred && deferred->owner == editor; }
QJsonArray Controller::status() const
{
  QJsonArray jobs;
  for (const auto& [key, attempt] : pending)
    jobs.append(QJsonObject{{"document_id", key}, {"attempt_id", attempt->request.metadata.value("attempt_id")},
      {"stage", attempt->job->currentStage()}, {"cancelling", attempt->job->cancellation->load()},
      {"mode", attempt->request.mode == Mode::Parameters ? "parameters" : attempt->request.mode == Mode::Preview ? "preview" : "render"}});
  return jobs;
}
void Controller::bind(EditorInterface *editor)
{
  if (!editor || boundEditors.contains(editor)) return;
  boundEditors.insert(editor);
  connect(editor, &QObject::destroyed, this, [this, editor] { boundEditors.remove(editor); });
  auto *parameters = editor->parameterWidget;
  connect(parameters, &ParameterWidget::metadataRequested, this, [this, editor] {
    request(editor, Mode::Parameters, true);
  });
  connect(editor, &EditorInterface::contentsChanged, this, [this, editor] {
    const auto id = editor->annotations->sessionId;
    cancelledRevisions.erase(id);
    // Snapshot captures all open buffers, but only a job that consumed a buffer
    // needs rejection at publication. Root edits can be cancelled immediately.
    const auto found = pending.find(id);
    if (found != pending.end()) found->second->job->cancellation->store(true);
  });
}
void Controller::request(EditorInterface *editor, Mode mode, bool automatic)
{
  QElapsedTimer snapshotTimer; snapshotTimer.start();
  if (!editor || window->isClosing || !window->tabManager->editorList.contains(editor)) return;
  bind(editor);
  const QString key = editor->annotations->sessionId;
  terminal.erase(key);
  const auto parameterRevision = editor->parameterWidget->evaluationRevision();
  const double time = window->animateWidget->getAnimTval();
  auto cancelled = cancelledRevisions.find(key);
  if (automatic && cancelled != cancelledRevisions.end() &&
      cancelled->second.source == editor->sourceRevision && cancelled->second.parameters == parameterRevision &&
      cancelled->second.time == time) return;
  if (!automatic) cancelledRevisions.erase(key);
  const auto previous = pending.find(key);
  // Reactive parameter work must not cancel a full evaluation which already
  // captures the same values. Genuine value changes invalidate it below.
  if (automatic && previous != pending.end() && previous->second->revision == editor->sourceRevision &&
      previous->second->parameterRevision == parameterRevision &&
      (mode == Mode::Parameters || previous->second->request.mode == mode) &&
      configurationCurrent(previous->second->request) &&
      previous->second->request.variables.time == time && !previous->second->job->cancellation->load()) return;
  // Freeze an explicit Preview/Render at its displayed camera before taking the snapshot.
  // Reactive refreshes and metadata work must not interrupt navigation.
  if (!automatic && mode != Mode::Parameters && editor == window->activeEditor)
    window->qglview->navigation()->cancel();
  auto attempt = std::make_shared<Pending>();
  attempt->owner = editor;
  attempt->revision = editor->sourceRevision;
  attempt->parameterRevision = parameterRevision;
  auto& request = attempt->request;
  request.mode = mode;
  request.prepareInspection = mode != Mode::Parameters && window->evaluationBinding->inspectionRequested(editor);
  request.filename = editor->filepath.toStdString();
  request.definitions = commandline_commands;
  auto colors = ColorMap::instance().findColorScheme(request.settings.colorscheme);
  request.colors = colors ? *colors : ColorMap::instance().defaultColorScheme();
  if (Settings::Settings::summaryCamera.value()) request.summary.emplace_back(RenderStatistic::CAMERA);
  if (Settings::Settings::summaryArea.value()) request.summary.emplace_back(RenderStatistic::AREA);
  if (Settings::Settings::summaryBoundingBox.value()) request.summary.emplace_back(RenderStatistic::BOUNDING_BOX);
  for (auto feature = Feature::begin(); feature != Feature::end(); ++feature)
    request.features[*feature] = (*feature)->is_enabled();
  request.variables = {mode != Mode::Render, window->animateWidget->getAnimTval(), window->qglview->cam};
  request.parameters = editor->parameterWidget->requestedValues();
  if (window->evaluationOverrides && window->evaluationOverrides->editor == editor) {
    request.variables = window->evaluationOverrides->variables;
    request.parameters = window->evaluationOverrides->parameters;
    request.strictParameters = true;
  }
  auto capture = [](EditorInterface *source) {
    const auto bytes = source->toPlainText().toUtf8();
    return SourceInput{source->filepath.isEmpty() ? "session:" + source->annotations->sessionId.toStdString()
      : source->filepath.toStdString(), std::string(bytes.constData(), size_t(bytes.size())), {},
      source->annotations->sessionId.toStdString(), source->sourceRevision};
  };
  request.root = capture(editor);
#ifdef ENABLE_PYTHON
  if (editor->filepath.endsWith(".py")) {
    if (!Feature::ExperimentalPythonEngine.is_enabled() ||
        !window->trust_python_file(request.filename, request.root.utf8)) {
      if (mode != Mode::Parameters) emit window->evaluationBinding->evaluationFinished(editor);
      return;
    }
    request.python = true;
    request.pythonEnvironment = venvBinDirFromSettings();
  }
  window->python_active = request.python;
#endif
  auto app = qobject_cast<OpenSCADApp *>(qApp);
  for (auto owner : app->windowManager.getWindows()) for (auto source : owner->tabManager->editorList) {
    request.editors.push_back(source == editor ? request.root : capture(source));
    attempt->editors[source->annotations->sessionId] = {source, source->sourceRevision};
  }
  const auto preferences = GlobalPreferences::inst();
  request.hardWarnings = preferences->getValue("advanced/enableHardwarnings").toBool();
  request.traceDepth = preferences->getValue("advanced/traceDepth").toUInt();
  request.traceParameters = preferences->getValue("advanced/enableTraceUsermoduleParameters").toBool();
  request.parameterCheck = preferences->getValue("advanced/enableParameterCheck").toBool();
  request.rangeCheck = preferences->getValue("advanced/enableParameterRangeCheck").toBool();
  request.csgLimit = preferences->getValue("advanced/openCSGLimit").toUInt();
  request.completionMode = QString::fromStdString(Settings::SettingsAutoCompletion::autocompleteMode.value());
  request.completionVariables = preferences->getValue("editor/autoCompleteIncludeVariables").toBool();
  request.completionModules = preferences->getValue("editor/autoCompleteIncludeModules").toBool();
  request.completionFunctions = preferences->getValue("editor/autoCompleteIncludeFunctions").toBool();
  request.metadata = {{"attempt_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
    {"document_id", key}, {"path", editor->filepath}, {"source_revision", QString::number(editor->sourceRevision)},
    {"time", request.variables.time}, {"backend", QString::fromStdString(renderBackend3DToString(request.settings.backend3D))},
    {"customizer", editor->parameterWidget->annotationValues()}, {"outcome", "evaluating"}};
  attempt->cameraRevision = window->annotationController->viewRevision();
  request.metadata["mode"] = mode == Mode::Parameters ? "parameters" : mode == Mode::Preview ? "preview" : "render";
  auto job = std::make_shared<Job>();
  attempt->job = job;
  request.validity = std::make_shared<InputValidity>();
  request.validity->cancellation = job->cancellation;
  for (const auto& [id, source] : attempt->editors) {
    const auto validity = request.validity;
    auto invalidate = [validity, id] { validity->invalidate(id.toStdString()); };
    attempt->inputConnections.push_back(connect(source.first.data(), &EditorInterface::contentsChanged, this, invalidate));
    attempt->inputConnections.push_back(connect(source.first.data(), &QObject::destroyed, this, invalidate));
    attempt->inputConnections.push_back(connect(source.first->annotations, &AgentAnnotations::Store::sessionChanged, this, invalidate));
  }
  job->key = key;
  job->automatic = automatic;
  job->compute = [attempt] { evaluate(attempt->request, *attempt->result, *attempt->job); };
  const QPointer<Controller> self(this);
  job->complete = [self, key, attempt] { if (self) self->finish(key, attempt); };
  if (previous != pending.end() && (previous->second->parameterRevision != parameterRevision ||
      previous->second->revision != editor->sourceRevision || !configurationCurrent(previous->second->request)))
    previous->second->job->cancellation->store(true);
  // Register before submit: replacing a queued job can complete it immediately.
  const auto displaced = previous == pending.end() ? nullptr : previous->second;
  pending[key] = attempt;
  attempt->lifecycle["snapshot_ms"] = double(snapshotTimer.nsecsElapsed()) / 1000000;
  if (!Service::instance().submit(job)) {
    if (displaced) pending[key] = displaced; else pending.erase(key);
    job->compute = {}; job->complete = {}; attempt->job.reset();
    return;
  }
  window->evaluationOverrides.reset();
  window->annotationController->observeBackgroundAttempt(request.metadata);
  if (mode != Mode::Parameters) {
    if (editor == window->activeEditor) {
      if (preferences->getValue("advanced/consoleAutoClear").toBool()) window->console->clear();
      if ((editor->isContentModified() || editor->hasViewChanges()) &&
          window->lastCompiledDoc.toUtf8().toStdString() != request.root.utf8) window->saveBackup();
    }
    window->resetCompileMessageCounts();
    window->evaluationBinding->invalidate(tr("Evaluation in progress; showing previous result"));
    window->inspectionPresentation->cancelDraft();
    window->annotationController->cancelDrawing();
    window->qglview->statistics()->setStale(true);
    window->resetMeasurementsState(false, "Evaluation in progress");
  }
  timer.start(); poll();
}
bool Controller::configurationCurrent(const Request& request) const
{
  if (request.settings.backend3D != RenderSettings::inst()->backend3D || request.definitions != commandline_commands) return false;
  for (const auto& [feature, enabled] : request.features) if (feature->is_enabled() != enabled) return false;
  const auto preferences = GlobalPreferences::inst();
  return request.hardWarnings == preferences->getValue("advanced/enableHardwarnings").toBool() &&
    request.traceDepth == preferences->getValue("advanced/traceDepth").toUInt() &&
    request.traceParameters == preferences->getValue("advanced/enableTraceUsermoduleParameters").toBool() &&
    request.parameterCheck == preferences->getValue("advanced/enableParameterCheck").toBool() &&
    request.rangeCheck == preferences->getValue("advanced/enableParameterRangeCheck").toBool() &&
    request.csgLimit == preferences->getValue("advanced/openCSGLimit").toUInt();
}
bool Controller::current(const Pending& attempt) const
{
  if (!attempt.owner || attempt.owner->sourceRevision != attempt.revision ||
      !window->tabManager->editorList.contains(attempt.owner.data()) ||
      attempt.owner->parameterWidget->evaluationRevision() != attempt.parameterRevision ||
      attempt.owner->filepath.toStdString() != attempt.request.filename ||
      !configurationCurrent(attempt.request)) return false;
  if (attempt.validating) {
    if (attempt.validationEpoch != watcherEpoch) return false;
    size_t count = 0;
    auto app = qobject_cast<OpenSCADApp *>(qApp);
    for (auto owner : app->windowManager.getWindows()) for (auto editor : owner->tabManager->editorList) {
      ++count;
      auto captured = std::find_if(attempt.validationEditors.begin(), attempt.validationEditors.end(),
        [editor](const auto& input) { return input.editorId == editor->annotations->sessionId.toStdString(); });
      if (captured == attempt.validationEditors.end() ||
          (attempt.validationUsedEditors.count(editor->annotations->sessionId.toStdString()) && captured->editorRevision != editor->sourceRevision) ||
          (!editor->filepath.isEmpty() && captured->fileKey != editor->filepath.toStdString())) return false;
    }
    if (count != attempt.validationEditors.size()) return false;
  }
  // Capturing conflicting editor buffers can fail before the input owner exists.
  // Revision/configuration checks still apply; failure delivery must not dereference it.
  if (!attempt.result->backing || !attempt.result->backing->inputs) return true;
  for (const auto& [key, input] : attempt.result->backing->inputs->consumed()) {
    if (!input.editorId) continue;
    auto captured = attempt.editors.find(QString::fromStdString(*input.editorId));
    if (captured == attempt.editors.end() || !captured->second.first ||
        captured->second.first->sourceRevision != captured->second.second) return false;
  }
  return true;
}
void Controller::cancel(EditorInterface *editor)
{
  if (!editor) return;
  const auto key = editor->annotations->sessionId;
  if (auto found = pending.find(key); found != pending.end()) found->second->cancelled = Job::Clock::now();
  cancelledRevisions[key] = {editor->sourceRevision, editor->parameterWidget->evaluationRevision(),
                             window->animateWidget->getAnimTval()};
  if (editor == window->activeEditor) window->animateWidget->pauseAnimation();
  Service::instance().cancel(key);
  if (deferred && deferred->owner == editor) deferred.reset();
  poll();
}
void Controller::poll()
{
  for (const auto& [key, attempt] : pending)
    if (!configurationCurrent(attempt->request)) attempt->job->cancellation->store(true);
  auto editor = window->activeEditor;
  const auto found = editor ? pending.find(editor->annotations->sessionId) : pending.end();
  const bool active = found != pending.end();
  const auto unwinding = editor ? Service::instance().runningFor(editor->annotations->sessionId) : nullptr;
  const auto last = editor ? terminal.find(editor->annotations->sessionId) : terminal.end();
  stop->setVisible(active || bool(unwinding)); label->setVisible(active || bool(unwinding) || last != terminal.end());
  if (active) label->setText(found->second->job->cancellation->load() ? tr("Cancelling…")
                                                                  : found->second->job->currentStage());
  if (active) drain(found->second, 64);
  else if (unwinding) label->setText(tr("Cancelling…") + " " + unwinding->currentStage());
  else if (last != terminal.end()) label->setText(last->second);
  if (pending.empty() && !unwinding) {
    timer.stop();
    if (!watcher.files().isEmpty()) watcher.removePaths(watcher.files());
  }
  else if (!timer.isActive()) timer.start();
}
void Controller::drain(const std::shared_ptr<Pending>& attempt, size_t limit)
{
  if (attempt->owner != window->activeEditor || attempt->job->cancellation->load()) return;
  std::vector<Message> batch;
  {
    std::lock_guard<std::mutex> lock(attempt->result->diagnosticMutex);
    const auto end = std::min(attempt->result->diagnostics.size(), attempt->delivered + limit);
    batch.assign(attempt->result->diagnostics.begin() + attempt->delivered,
                 attempt->result->diagnostics.begin() + end);
    attempt->delivered = end;
  }
  const QJsonObject context{{"attempt_id", attempt->request.metadata.value("attempt_id")},
    {"document_id", attempt->request.metadata.value("document_id")}, {"scope", "evaluation"}};
  for (const auto& message : batch) {
    window->consoleOutput(message);
    window->errorLogOutput(message);
    window->annotationController->observeDiagnostic(message, context);
  }
}
void Controller::finish(const QString& key, const std::shared_ptr<Pending>& attempt)
{
  const auto milliseconds = [](auto duration) { return double(std::chrono::duration_cast<std::chrono::microseconds>(duration).count()) / 1000; };
  const auto job = attempt->job;
  if (job->started != Job::Clock::time_point{}) {
    attempt->lifecycle[attempt->validating ? "validation_queue_ms" : "queue_ms"] = milliseconds(job->started - job->queued);
    attempt->lifecycle[attempt->validating ? "validation_ms" : "worker_ms"] = milliseconds(job->finished - job->started);
  }
  if (attempt->cancelled != Job::Clock::time_point{})
    attempt->lifecycle["cancel_to_stop_ms"] = std::max(0.0, milliseconds(job->finished - attempt->cancelled));
  auto found = pending.find(key);
  const bool latest = found != pending.end() && found->second == attempt;
  if (latest && !attempt->validating && attempt->job->succeeded && attempt->result->success &&
      !attempt->job->cancellation->load() && current(*attempt)) {
    validate(attempt);
    return;
  }
  auto metadata = attempt->request.metadata;
  metadata["outcome"] = !latest ? "superseded" : attempt->job->cancellation->load() ? "cancelled" :
    !current(*attempt) ? "stale" : attempt->job->succeeded && attempt->result->success ? "completed" : "failed";
  metadata["timings_ms"] = attempt->result->timings;
  metadata["lifecycle_ms"] = attempt->lifecycle;
  metadata["errors"] = attempt->result->errors;
  metadata["warnings"] = attempt->result->warnings;
  auto report = [&] {
    if (!attempt->publicationMetadata.isEmpty()) metadata = attempt->publicationMetadata;
    if (!attempt->publicationError.isEmpty()) {
      metadata["outcome"] = "failed";
      metadata["stage"] = "publication";
      metadata["error"] = attempt->publicationError;
    }
    window->annotationController->observeBackgroundAttempt(metadata);
    if (!attempt->terminalReported) {
      attempt->terminalReported = true;
      emit attemptFinished(metadata);
      if (latest && attempt->request.mode != Mode::Parameters && attempt->owner)
        emit window->evaluationBinding->evaluationFinished(attempt->owner);
    }
  };
  if (!latest) { report(); return; }
  terminal[key] = attempt->job->cancellation->load() ? tr("Cancelled") :
    !current(*attempt) ? tr("Superseded") : attempt->job->succeeded && attempt->result->success ? tr("Completed") : tr("Failed");
  QTimer::singleShot(2500, this, [this, key] { terminal.erase(key); poll(); });
  pending.erase(found);
  if (attempt->job->cancellation->load() || !current(*attempt)) { report(); poll(); return; }
  if (attempt->owner != window->activeEditor && attempt->request.mode != Mode::Parameters) {
    deferred = attempt;
    metadata["outcome"] = "deferred";
  }
  else install(attempt);
  report();
  poll();
}
void Controller::activate(EditorInterface *editor)
{
  bind(editor);
  if (deferred && deferred->owner == editor) {
    auto result = std::exchange(deferred, {});
    result->validating = false;
    if (current(*result) && !pending.count(editor->annotations->sessionId)) {
      pending[editor->annotations->sessionId] = result;
      validate(result);
    }
  }
  poll();
}
void Controller::validate(const std::shared_ptr<Pending>& attempt)
{
  // Watch before sampling disk bytes. A notification during validation prevents
  // installation; strict action validation still guards delayed notifications.
  QStringList paths;
  for (const auto& [key, input] : attempt->result->backing->inputs->consumed())
    if (!input.editorId && !watcher.files().contains(QString::fromStdString(key))) paths.append(QString::fromStdString(key));
  if (!paths.isEmpty() && !watcher.addPaths(paths).isEmpty()) {
    attempt->job->error = tr("An evaluation dependency could not be watched");
    attempt->job->succeeded = false;
    finish(attempt->owner->annotations->sessionId, attempt);
    return;
  }
  attempt->validationEditors.clear();
  auto app = qobject_cast<OpenSCADApp *>(qApp);
  for (auto owner : app->windowManager.getWindows()) for (auto editor : owner->tabManager->editorList) {
    const auto bytes = editor->toPlainText().toUtf8();
    attempt->validationEditors.push_back({editor->filepath.isEmpty() ?
      "session:" + editor->annotations->sessionId.toStdString() : editor->filepath.toStdString(),
      std::string(bytes.constData(), size_t(bytes.size())), {}, editor->annotations->sessionId.toStdString(), editor->sourceRevision});
  }
  attempt->validationEpoch = watcherEpoch;
  attempt->validating = true;
  auto job = std::make_shared<Job>();
  job->key = attempt->owner->annotations->sessionId;
  job->automatic = attempt->job->automatic;
  job->cancellation = attempt->job->cancellation;
  attempt->job = job;
  job->compute = [attempt] {
    attempt->job->setStage("Validating inputs");
    attempt->validationUsedEditors.clear();
    auto editors = attempt->validationEditors;
    for (auto& input : editors) {
      evaluationCheckpoint();
      if (input.fileKey.rfind("session:", 0) != 0) input.fileKey = EvaluationInputs::fileKey(input.fileKey);
      input.fingerprint = EvaluationInputs::fingerprint(input.utf8);
    }
    for (const auto& [key, input] : attempt->result->backing->inputs->consumed()) {
      evaluationCheckpoint();
      if (input.editorId) {
        const auto owner = std::find_if(editors.begin(), editors.end(),
          [&](const auto& live) { return live.editorId == input.editorId; });
        if (owner == editors.end() || owner->fileKey != key || owner->editorRevision != input.editorRevision)
          throw EvaluationInputError("An evaluation editor changed identity");
      }
      bool matched = false;
      for (const auto& live : editors) if (live.fileKey == key) {
        matched = true;
        if (live.editorId) attempt->validationUsedEditors.insert(*live.editorId);
        if (live.fingerprint != input.fingerprint) throw EvaluationInputError("An open evaluation input changed");
      }
      if (!matched && EvaluationInputs::fingerprint(EvaluationInputs::diskBytes(key)) != input.fingerprint)
        throw EvaluationInputError("An evaluation file changed");
    }
  };
  const QPointer<Controller> self(this);
  const auto key = job->key;
  job->complete = [self, key, attempt] { if (self) self->finish(key, attempt); };
  if (!Service::instance().submit(job)) {
    job->cancellation->store(true);
    finish(key, attempt);
    job->compute = {}; job->complete = {};
  }
  timer.start();
}
void Controller::install(const std::shared_ptr<Pending>& attempt)
{
  QElapsedTimer publicationTimer; publicationTimer.start();
  auto editor = attempt->owner.data();
  if (!editor || !current(*attempt)) return;
  auto& result = *attempt->result;
  drain(attempt, 2048);
  // This tracks the last attempted source for edit/backup bookkeeping, not
  // the retained display. Failed current evaluations still attempted it.
  if (attempt->request.mode != Mode::Parameters && editor == window->activeEditor)
    window->lastCompiledDoc = QString::fromUtf8(attempt->request.root.utf8.data(), int(attempt->request.root.utf8.size()));
  if (!attempt->job->succeeded || !result.success) {
    if (editor == window->activeEditor) {
      if (!attempt->job->error.isEmpty()) window->consoleOutput(Message(attempt->job->error.toStdString(), message_group::Error));
      if (result.parserError >= 0) editor->highlightError(result.parserError);
      window->compileErrors = result.errors + int(!attempt->job->error.isEmpty());
      window->compileWarnings = result.warnings;
      window->updateCompileResult();
      window->qglview->statistics()->evaluationFailed();
#ifdef ENABLE_GUI_TESTS
      emit window->compilationDone(nullptr);
#endif
    }
    return;
  }
  editor->parameterWidget->installEvaluation(std::move(result.parameters), attempt->request.root.utf8,
                                            result.backing);
  const double customizerMs = double(publicationTimer.nsecsElapsed()) / 1000000;
  editor->resetHighlighting();
  editor->setIndicator(result.backing->parsedRoot->indicatorData);
  if (auto scintilla = dynamic_cast<ScintillaEditor *>(editor)) scintilla->setCompletionNames(std::move(result.completion));
  const double editorMs = double(publicationTimer.nsecsElapsed()) / 1000000;
  if (attempt->request.mode == Mode::Parameters) return;
  if (editor != window->activeEditor) return;
  // Construct presentation objects before replacing any last-good scene state.
  // No event pumping or consumer notification is allowed inside this swap.
  window->qglview->makeCurrent();
  std::shared_ptr<Renderer> preview, thrown, rendered;
  try {
    if (attempt->request.mode == Mode::Preview) {
#ifdef ENABLE_OPENCSG
      if (result.products->size() <= attempt->request.csgLimit)
        preview = std::make_shared<OpenCSGRenderer>(result.products, result.highlights, result.background,
                                                    result.previewTriangles);
#endif
      thrown = std::make_shared<ThrownTogetherRenderer>(result.products, result.highlights, result.background,
                                                        result.previewTriangles);
    } else if (result.geometry) {
      if (result.usePolySet) rendered = std::make_shared<PolySetRenderer>(std::move(result.preparedGeometry));
#ifndef USE_POLYSET_FOR_CGAL
      else rendered = std::make_shared<CGALRenderer>(std::move(result.preparedCGAL));
#endif
    }
  } catch (const std::exception& error) {
    window->consoleOutput(Message(error.what(), message_group::Error));
    window->qglview->statistics()->evaluationFailed();
    attempt->publicationError = QString::fromUtf8(error.what());
    terminal[editor->annotations->sessionId] = tr("Failed");
#ifdef ENABLE_GUI_TESTS
    emit window->compilationDone(nullptr);
#endif
    return;
  }
  QSignalBlocker viewSignals(window->qglview), bindingSignals(window->evaluationBinding),
    annotationSignals(window->annotationController);
  const double rendererMs = double(publicationTimer.nsecsElapsed()) / 1000000;
  auto retired = std::make_shared<std::tuple<decltype(window->evaluationBacking), decltype(window->absoluteRootNode),
    decltype(window->rootNode), decltype(window->rootGeom), decltype(window->csgRoot), decltype(window->rootProduct),
    decltype(window->normalizedRoot), decltype(window->highlightsProducts), decltype(window->backgroundProducts)>>(
      window->evaluationBacking, window->absoluteRootNode, window->rootNode, window->rootGeom,
      window->csgRoot, window->rootProduct, window->normalizedRoot, window->highlightsProducts, window->backgroundProducts);
  window->compileErrors = result.errors; window->compileWarnings = result.warnings;
  if (result.omittedMessages) window->consoleOutput(Message(std::to_string(result.omittedMessages) + " additional messages omitted"));
  window->isPreview = attempt->request.mode == Mode::Preview;
  window->evaluationInputs = result.backing->inputs;
  window->evaluationBacking = result.backing;
  window->rootFile = window->parsedFile = result.backing->parsedRoot;
  window->absoluteRootNode = result.absoluteRoot; window->rootNode = result.root;
  window->tree.setRoot(result.root); window->tree.setDocumentPath(result.scene->documentPath);
  window->evaluatedScene = result.scene;
  window->renderedEditor = editor;
  if (attempt->cameraRevision == window->annotationController->viewRevision()) {
    window->qglview->cam = result.camera;
    window->viewportControlWidget->cameraChanged();
  }
  window->qglview->makeCurrent();
  window->csgRoot = result.csg; window->normalizedRoot = result.normalized;
  window->rootProduct = result.products; window->highlightsProducts = result.highlights;
  window->backgroundProducts = result.background;
  window->rootGeom = result.geometry;
  if (window->isPreview) {
#ifdef ENABLE_OPENCSG
    window->previewRenderer = std::move(preview);
#endif
    window->thrownTogetherRenderer = std::move(thrown);
    window->selectPreviewViewMode();
    window->resetMeasurementsState(false, "Render to enable measurements");
    if (window->animateWidget->dumpPictures()) {
      window->animateWidget->nextFrame();
      if (window->animateWidget->dumpPictures() &&
          !window->animateWidget->saveFrame(window->qglview->grabFrame())) {
        window->animateWidget->e_dump->setChecked(false);
        window->animateWidget->pauseAnimation();
      }
    }
  } else {
    window->geomRenderer = std::move(rendered);
    window->viewModeRender();
    window->resetMeasurementsState(bool(result.geometry), "Click to start measuring");
    editor->contentsRendered = true;
    const auto preferences = GlobalPreferences::inst();
    if (preferences->getValue("advanced/enableSoundNotification").toBool() &&
        preferences->getValue("advanced/timeThresholdOnRenderCompleteSound").toUInt() <= result.timings.value("Ready").toDouble() / 1000)
      window->renderCompleteSoundEffect->play();
  }
  auto metadata = attempt->request.metadata;
  metadata["customizer"] = editor->parameterWidget->annotationValues();
  const double sceneMs = double(publicationTimer.nsecsElapsed()) / 1000000;
  metadata["timings_ms"] = result.timings;
  metadata["lifecycle_ms"] = attempt->lifecycle;
  metadata["duration_ms"] = result.timings.value("Ready");
  metadata["source_sha256"] = QString::fromStdString(result.backing->inputs->root().fingerprint);
  metadata["empty"] = window->isPreview ?
    result.products->size() == 0 && result.highlights->size() == 0 && result.background->size() == 0 :
    !result.geometry || result.geometry->isEmpty();
  attempt->publicationMetadata = window->annotationController->finishBackgroundEvaluation(
    editor, metadata, std::move(result.inspection));
  viewSignals.unblock(); bindingSignals.unblock(); annotationSignals.unblock();
  emit window->qglview->rendererReplaced();
  emit window->evaluationBinding->installed();
  emit window->annotationController->changed();
  Service::instance().retire([retired] {});
  installedAt = Job::Clock::now();
  awaitingFrame = {{"attempt_id", metadata.value("attempt_id")},
    {"document_id", metadata.value("document_id")},
    {"customizer_ms", customizerMs}, {"editor_ms", editorMs - customizerMs},
    {"renderer_ms", rendererMs - editorMs}, {"scene_ms", sceneMs - rendererMs},
    {"metadata_ms", double(publicationTimer.nsecsElapsed()) / 1000000 - sceneMs},
    {"publication_ms", double(publicationTimer.nsecsElapsed()) / 1000000}};
  emit window->displayedDocumentChanged();
  window->updateCompileResult();
  window->qglview->update();
  window->qglview->statistics()->setStale(false);
#ifdef ENABLE_GUI_TESTS
  emit window->compilationDone(window->rootFile.get());
#endif
}
}
