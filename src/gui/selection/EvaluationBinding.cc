#include "gui/selection/EvaluationBinding.h"
#include "gui/MainWindow.h"
#include "gui/OpenSCADApp.h"
#include "gui/TabManager.h"
#include "gui/QGLView.h"
#include "glview/preview/ThrownTogetherRenderer.h"
#include "gui/parameter/ParameterWidget.h"
#include "gui/annotations/AnnotationStore.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/QSettingsCached.h"
#include <QSet>
#include <algorithm>

namespace {
QSet<EditorInterface *> openEditors(MainWindow& current)
{
  auto editors = current.tabManager->editorList;
  if (auto app = qobject_cast<OpenSCADApp *>(QCoreApplication::instance()))
    for (auto window : app->windowManager.getWindows()) editors.unite(window->tabManager->editorList);
  return editors;
}
SourceInput editorInput(EditorInterface& editor)
{
  const auto bytes = editor.toPlainText().toUtf8();
  return {editor.filepath.isEmpty() ? "session:" + editor.annotations->sessionId.toStdString()
    : EvaluationInputs::fileKey(std::filesystem::path(editor.filepath.toStdString())),
    std::string(bytes.constData(), static_cast<size_t>(bytes.size())), {},
    editor.annotations->sessionId.toStdString(), editor.sourceRevision};
}
}

namespace Selection {
bool EvaluationBinding::inspectionRequested(EditorInterface *editor) const
{
  if (QSettingsCached().value("advanced/alwaysPrepareInspector", false).toBool()) return true;
  return editor && std::any_of(inspectionRequests.begin(), inspectionRequests.end(),
    [editor](const auto& entry) { return entry.second.editor == editor; });
}
bool EvaluationBinding::inspectionReady() const
{
  return retainedScene && retainedScene->inspectionPrepared && validate(installedStamp);
}
void EvaluationBinding::requestInspection(QObject *consumer, EditorInterface *editor)
{
  if (!consumer || !editor) return;
  releaseInspection(consumer);
  inspectionRequests.emplace(consumer, InspectionRequest{editor,
    connect(consumer, &QObject::destroyed, this, [this, consumer] { releaseInspection(consumer); })});
}
void EvaluationBinding::releaseInspection(QObject *consumer)
{
  const auto found = inspectionRequests.find(consumer);
  if (found == inspectionRequests.end()) return;
  disconnect(found->second.destroyed);
  inspectionRequests.erase(found);
}
EvaluationBinding::EvaluationBinding(MainWindow *window) : QObject(window), window(window)
{
  if (auto app = qobject_cast<OpenSCADApp *>(QCoreApplication::instance()))
    connect(&app->windowManager, &WindowManager::windowsChanged, this,
      &EvaluationBinding::editorsChanged, Qt::QueuedConnection);
  connect(&inputWatcher, &QFileSystemWatcher::fileChanged, this, [this] {
    diskInputsChanged = true;
    if (fresh && retainedScene) invalidate(tr("An evaluation file changed"));
  });
  connect(window->qglview, &QGLView::rendererReplaced, this, &EvaluationBinding::rendererChanged);
  connect(window->tabManager, &TabManager::currentEditorChanged, this,
    [this](EditorInterface *) { emit invalidated(); });
  connect(window->tabManager, &TabManager::editorAboutToClose, this, [this](EditorInterface *editor) {
    if (owner == editor) invalidate(tr("Result document closed"));
    else if (fresh && retainedScene)
      for (const auto& [key, input] : retainedScene->inputs->consumed())
        if (input.editorId == editor->annotations->sessionId.toStdString()) {
          invalidate(tr("An evaluation dependency closed")); break;
        }
  });
}

std::shared_ptr<EvaluationInputs> EvaluationBinding::capture(MainWindow& window, EditorInterface& root)
{
  std::vector<SourceInput> editors;
  for (auto editor : openEditors(window))
    if (!editor->filepath.isEmpty() || editor == &root) editors.push_back(editorInput(*editor));
  return std::make_shared<EvaluationInputs>(editorInput(root), std::move(editors), EvaluationInputs::ReadPolicy::CaptureMissing);
}

bool EvaluationBinding::inputsCurrent(const EvaluationInputs& inputs, MainWindow& window, QString *detail)
{
  auto fail = [detail](const QString& message) { if (detail) *detail = message; return false; };
  try {
    // Capture each live editor once, rather than copying/canonicalizing it for
    // every included file. Still compare exact bytes at every action boundary.
    std::vector<SourceInput> editors;
    for (auto editor : openEditors(window)) {
      auto live = editorInput(*editor);
      live.fingerprint = EvaluationInputs::fingerprint(live.utf8);
      editors.push_back(std::move(live));
    }
    for (const auto& [key, input] : inputs.validationInputs()) {
      bool matched = false;
      for (const auto& live : editors) {
        if (live.fileKey != key) continue;
        matched = true;
        if (live.fingerprint != input.fingerprint)
          return fail(QObject::tr("An open evaluation input changed: %1").arg(QString::fromStdString(key)));
      }
      for (const auto& captured : inputs.capturedEditors()) {
        if (captured.fileKey != key) continue;
        bool found = false;
        for (const auto& live : editors) {
          if (live.editorId == captured.editorId) {
            found = live.fileKey == key && live.editorRevision == captured.editorRevision;
            break;
          }
        }
        if (!found) return fail(QObject::tr("An evaluation editor changed or closed"));
      }
      if (!matched && EvaluationInputs::fingerprint(EvaluationInputs::diskBytes(std::filesystem::path(key))) != input.fingerprint)
        return fail(QObject::tr("An evaluation file changed: %1").arg(QString::fromStdString(key)));
    }
  } catch (const std::exception& error) { return fail(QString::fromUtf8(error.what())); }
  return true;
}

void EvaluationBinding::install(std::shared_ptr<const EvaluatedScene> scene, EditorInterface *editor,
                                const QString& resultId, const QString& configuration)
{
  retainedScene = std::move(scene);
  diskInputsChanged = false;
  owner = editor;
  installedStamp = {editor->annotations->sessionId, resultId, editor->sourceFingerprint, configuration,
    editor->sourceRevision, viewportGeneration};
  fresh = retainedScene && window && owner;
  reason = fresh ? QString() : tr("Evaluation inputs changed before result installation");
  auto retainRenderer = [this](const std::shared_ptr<Renderer>& renderer) {
    renderers[renderer.get()] = {renderer, owner, retainedScene, installedStamp};
  };
  retainRenderer(window->qglview->renderer);
  if (window->isPreview) {
#ifdef ENABLE_OPENCSG
    retainRenderer(window->previewRenderer);
#endif
    retainRenderer(window->thrownTogetherRenderer);
  }
  for (auto entry = renderers.begin(); entry != renderers.end();)
    if (entry->first && entry->second.renderer.expired()) entry = renderers.erase(entry); else ++entry;
  observeInputs();
  emit installed();
}

void EvaluationBinding::invalidate(const QString& message)
{
  fresh = false;
  for (auto& [renderer, binding] : renderers)
    if (binding.stamp.resultId == installedStamp.resultId) binding.eligible = false;
  reason = message;
  emit invalidated();
}

void EvaluationBinding::rendererChanged()
{
  ++viewportGeneration;
  const auto renderer = window->qglview->getRenderer();
  auto found = renderers.find(renderer);
  if (found != renderers.end() && (!renderer || !found->second.renderer.expired())) {
    owner = found->second.owner;
    retainedScene = found->second.scene;
    installedStamp = found->second.stamp;
    fresh = found->second.eligible && owner && retainedScene;
    fresh = current();
  } else fresh = false;
  installedStamp.viewportGeneration = viewportGeneration;
  observeInputs();
  emit invalidated();
}

bool EvaluationBinding::replacePresentationRenderer(std::shared_ptr<Renderer> renderer, const Stamp& expected)
{
  if (!window || !(expected == installedStamp)) return false;
  renderers[renderer.get()] = {renderer, owner, retainedScene, installedStamp, fresh};
  window->annotationController->registerPresentationRenderer(renderer, expected.resultId);
  if (window->qglview->renderer == renderer) rendererChanged();
  else window->qglview->setRenderer(std::move(renderer));
  window->qglview->update();
  return true;
}

void EvaluationBinding::editorsChanged()
{
  observeInputs();
  if (retainedScene && fresh)
    invalidate(tr("An open evaluation input changed"));
}

void EvaluationBinding::observeInputs()
{
  for (auto connection : inputConnections) QObject::disconnect(connection);
  inputConnections.clear();
  if (!inputWatcher.files().isEmpty()) inputWatcher.removePaths(inputWatcher.files());
  if (!retainedScene) return;
  QSet<MainWindow *> windows{window.data()};
  if (auto app = qobject_cast<OpenSCADApp *>(QCoreApplication::instance()))
    windows.unite(app->windowManager.getWindows());
  for (auto openWindow : windows)
    inputConnections.push_back(connect(openWindow->tabManager, &TabManager::tabCountChanged,
      this, &EvaluationBinding::editorsChanged, Qt::QueuedConnection));
  for (auto editor : openEditors(*window)) {
    bool used = false;
    for (const auto& captured : retainedScene->inputs->capturedEditors())
      if (captured.editorId == editor->annotations->sessionId.toStdString() &&
          retainedScene->inputs->validationInputs().count(captured.fileKey)) { used = true; break; }
    if (!used) continue;
    inputConnections.push_back(connect(editor, &EditorInterface::contentsChanged, this,
      [this] { invalidate(tr("An evaluation input was edited")); }));
    inputConnections.push_back(connect(editor, &QObject::destroyed, this,
      [this] { invalidate(tr("An evaluation input editor closed")); }));
    inputConnections.push_back(connect(editor->annotations, &AgentAnnotations::Store::sessionChanged, this,
      [this] { invalidate(tr("An evaluation document was replaced")); }));
    if (editor == owner)
      inputConnections.push_back(connect(editor->parameterWidget, &ParameterWidget::metadataRequested, this,
        [this] { invalidate(tr("Evaluation parameters changed")); }));
  }
  QStringList paths;
  for (const auto& [key, input] : retainedScene->inputs->validationInputs()) {
    const auto path = QString::fromStdString(key);
    if (!input.editorId && !path.startsWith("session:")) paths.append(path);
  }
  if (!paths.isEmpty() && !inputWatcher.addPaths(paths).isEmpty()) {
    fresh = false;
    reason = tr("An evaluation dependency could not be watched");
  }
}

bool EvaluationBinding::current() const
{
  return fresh && window && owner && retainedScene &&
    owner->annotations->sessionId == installedStamp.documentId &&
    owner->sourceRevision == installedStamp.sourceRevision &&
    owner->sourceFingerprint == installedStamp.sourceFingerprint;
}
bool EvaluationBinding::displayCurrent(const Stamp& expected) const
{
  return !hypotheticalPixels && installedStamp == expected && current() && window->activeEditor == owner;
}
bool EvaluationBinding::modelDisplayCurrent(const Stamp& expected) const
{
  auto model = expected, installed = installedStamp;
  model.viewportGeneration = installed.viewportGeneration = 0;
  return model == installed && current() && window->activeEditor == owner;
}
std::shared_ptr<const EvaluatedScene> EvaluationBinding::displayScene(const Stamp& expected) const
{
  return displayCurrent(expected) ? retainedScene : nullptr;
}
bool EvaluationBinding::validate(const Stamp& expected, QString *detail) const
{
  const bool valid = displayCurrent(expected) &&
    inputsCurrent(*retainedScene->inputs, *window, detail);
  if (!valid && detail) *detail = reason.isEmpty() ? tr("Stale or wrong-document evaluated result") : reason;
  return valid;
}
bool EvaluationBinding::validateModel(const Stamp& expected, QString *detail) const
{
  const bool valid=modelDisplayCurrent(expected) &&
    inputsCurrent(*retainedScene->inputs, *window, detail);
  if(!valid && detail)*detail=reason.isEmpty()?tr("Stale or wrong-document model inputs"):reason;
  return valid;
}
std::shared_ptr<const EvaluatedScene> EvaluationBinding::scene(const Stamp& expected) const
{
  return validate(expected) ? retainedScene : nullptr;
}
}
