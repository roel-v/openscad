#include "AnnotationController.h"
#include "gui/evaluation/EvaluationController.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/ViewportToolRouter.h"
#include "Feature.h"
#include "openscad.h"
#include "AnnotationBridge.h"
#include "AgentModel.h"
#include "gui/Animate.h"
#include "gui/Dock.h"
#include "gui/Editor.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/Preferences.h"
#include "gui/CameraNavigation.h"
#include "gui/SavedViews.h"
#include "glview/RenderSettings.h"
#include "core/SourceFileCache.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QUuid>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace AgentAnnotations {
namespace {
QString uuid()
{
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString sourceHash(EditorInterface *editor)
{
  return editor ? editor->sourceFingerprint : QString();
}
QString label(EditorInterface *editor)
{
  return editor && !editor->filepath.isEmpty() ? editor->filepath : QString("Untitled.scad");
}
class Overlay : public QWidget
{
public:
  Drawing *drawing;
  Overlay(QWidget *parent, Drawing *drawing) : QWidget(parent), drawing(drawing)
  {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
  }
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
    drawing->paint(painter, size());
  }
};
}  // namespace

Controller::Controller(MainWindow *mainWindow)
  : QObject(mainWindow),
    windowId(uuid()),
    viewportId(uuid()),
    window(mainWindow),
    view(mainWindow->qglview)
{
  dock = new Dock(window);
  dock->setObjectName("annotationsDock");
  dock->setName(tr("Annotations"));
  panel = new QWidget(dock);
  auto layout = new QVBoxLayout(panel);
  layout->setAlignment(Qt::AlignTop);
  ownerLabel = new QLabel(panel);
  ownerLabel->setWordWrap(true);
  ownerLabel->setTextFormat(Qt::PlainText);
  layout->addWidget(ownerLabel);
  auto annotate = new QPushButton(tr("Annotate issue"), panel);
  annotate->setObjectName("annotationStart");
  layout->addWidget(annotate);
  connect(annotate, &QPushButton::clicked, this, &Controller::startDrawing);
  auto blank = new QPushButton(tr("Use active document for blank view"), panel);
  layout->addWidget(blank);
  connect(blank, &QPushButton::clicked, this, [this] {
    if (!busy() && !view->getRenderer()) {
      displayedOwner = window->activeEditor;
      result = {};
      refresh();
      emit changed();
    }
  });
  search = new QLineEdit(panel);
  search->setPlaceholderText(tr("Search annotations"));
  layout->addWidget(search);
  list = new QListWidget(panel);
  list->setObjectName("annotationList");
  list->setMinimumHeight(65);
  list->setMaximumHeight(150);
  layout->addWidget(list);
  imageLabel = new QLabel(panel);
  imageLabel->setAlignment(Qt::AlignCenter);
  imageLabel->setMinimumHeight(100);
  imageLabel->setMinimumHeight(180);
  imageLabel->setMaximumHeight(240);
  layout->addWidget(imageLabel);
  commentLabel = new QLabel(panel);
  commentLabel->setWordWrap(true);
  commentLabel->setTextFormat(Qt::PlainText);
  commentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(commentLabel);
  auto buttons = new QGridLayout;
  auto edit = new QPushButton(tr("Edit comment"), panel), remove = new QPushButton(tr("Delete"), panel);
  auto restore = new QPushButton(tr("Restore view here"), panel),
       save = new QPushButton(tr("Save owner document"), panel);
  buttons->addWidget(edit, 0, 0);
  buttons->addWidget(remove, 0, 1);
  buttons->addWidget(restore, 1, 0);
  buttons->addWidget(save, 1, 1);
  layout->addLayout(buttons);
  connect(search, &QLineEdit::textChanged, this, &Controller::refresh);
  connect(list, &QListWidget::currentRowChanged, this, &Controller::showSelected);
  connect(save, &QPushButton::clicked, this, [this] {
    if (owner() && !drafting()) window->tabManager->save(owner());
  });
  connect(edit, &QPushButton::clicked, this, [this] {
    if (!owner() || !list->currentItem() || drafting()) return;
    QPointer<EditorInterface> target = owner();
    QString id = list->currentItem()->data(Qt::UserRole).toString();
    const auto record = target->annotations->find(id);
    if (!record) return;
    const auto revision = target->annotations->revision();
    QDialog dialog(window);
    dialog.setWindowTitle(tr("Edit annotation comment"));
    QVBoxLayout layout(&dialog);
    QPlainTextEdit text(record->metadata.value("comment").toString(), &dialog);
    text.setObjectName("annotationEditComment");
    layout.addWidget(&text);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout.addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    text.setFocus();
    if (dialog.exec() == QDialog::Accepted && target && target->annotations->revision() == revision) {
      if (!target->annotations->edit(id, text.toPlainText()))
        QMessageBox::warning(window, tr("Annotations"), tr("Comment or undo history limit exceeded."));
    }
    panel->setFocus();
  });
  connect(remove, &QPushButton::clicked, this, [this] {
    if (!owner() || !list->currentItem() || drafting()) return;
    QPointer<EditorInterface> target = owner();
    auto revision = target->annotations->revision();
    QString id = list->currentItem()->data(Qt::UserRole).toString();
    if (QMessageBox::question(window, tr("Delete annotation"),
                              tr("Delete this annotation and its screenshot?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes &&
        target && target->annotations->revision() == revision)
      target->annotations->remove(id);
    panel->setFocus();
  });
  connect(restore, &QPushButton::clicked, this, [this] {
    if (!owner() || !list->currentItem()) return;
    auto record = owner()->annotations->find(list->currentItem()->data(Qt::UserRole).toString());
    QString error;
    if (record && !setCamera(record->metadata.value("camera").toObject(), &error))
      QMessageBox::warning(window, tr("Restore view"), error);
  });
  panel->setFocusPolicy(Qt::StrongFocus);
  panel->installEventFilter(this);
  auto scroll = new QScrollArea(dock);
  scroll->setWidgetResizable(true);
  scroll->setWidget(panel);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  dock->setWidget(scroll);
  window->addDockWidget(Qt::RightDockWidgetArea, dock);
  dock->hide();
  imageLabel->installEventFilter(this);
  overlay = new Overlay(view, &drawing);
  overlay->hide();
  palette = new QWidget(view);
  palette->setAutoFillBackground(true);
  palette->setCursor(Qt::ArrowCursor);
  palette->setAttribute(Qt::WA_NoMousePropagation);
  palette->setObjectName("annotationPalette");
  auto colors = new QGridLayout(palette);
  colors->addWidget(new QLabel(tr("Enter: comment · Esc: cancel"), palette), 0, 0, 1, 6);
  const QStringList values{"#ffcf33", "#ff5533", "#ef4da8", "#ab67e0", "#6060ff", "#2299ee",
                           "#33cccc", "#44bb66", "#aacc33", "#ffffff", "#aaaaaa", "#222222"};
  int index = 0;
  for (const auto& value : values) {
    auto button = new QPushButton(palette);
    button->setFixedSize(25, 25);
    button->setToolTip(value);
    button->setStyleSheet("background-color:" + value + ";border:1px solid #777;");
    colors->addWidget(button, 1 + index / 6, index % 6);
    ++index;
    connect(button, &QPushButton::clicked, this, [this, value] {
      color = QColor(value);
      view->setFocus();
    });
  }
  auto undo = new QPushButton(tr("Undo"), palette), redo = new QPushButton(tr("Redo"), palette);
  colors->addWidget(undo, 3, 0, 1, 3);
  colors->addWidget(redo, 3, 3, 1, 3);
  connect(undo, &QPushButton::clicked, this, [this] {
    drawing.undo();
    updateLock();
    view->setFocus();
  });
  connect(redo, &QPushButton::clicked, this, [this] {
    drawing.redo();
    updateLock();
    view->setFocus();
  });
  palette->adjustSize();
  view->composeAnnotations=[this](QImage& image) {
    if (drafting()) { QPainter painter(&image); drawing.paint(painter,image.size(),view->devicePixelRatioF()); }
  };
  palette->move(12, 12);
  palette->hide();
  displayedOwner = window->activeEditor;
  view->installEventFilter(this);
  qApp->installEventFilter(this);
  originalSaveText = window->fileActionSave->text();
  originalUndoText = window->editActionUndo->text();
  connect(qApp, &QApplication::focusChanged, this, [this] { updateActions(); });
  connect(view, &QGLView::cameraChanged, this, [this] {
    viewRevision();
    if (state == DrawingState) {
      if (drawing.marked()) view->cam = lockedCamera;
      else {
        drawing.navigation();
        lockedCamera = view->cam;
      }
    }
    emit cameraStateChanged();
  });
  connect(view->navigation(), &CameraNavigation::displayChanged, this, [this] { viewRevision(); emit cameraStateChanged(); });
  connect(view, &QGLView::frameAboutToRender, this, [this] {
    viewRevision();
    if (drafting() && display() != draftDisplay) {
      cancelDrawing();
      return;
    }
    if (state != DrawingState) return;
    if (!drawing.marked() &&
        (view->cam.object_trans != lockedCamera.object_trans ||
         view->cam.object_rot != lockedCamera.object_rot ||
         view->cam.viewer_distance != lockedCamera.viewer_distance ||
         view->cam.fov != lockedCamera.fov || view->cam.projection != lockedCamera.projection)) {
      drawing.navigation();
      lockedCamera = view->cam;
    }
  });
  connect(view, &QGLView::resized, this, [this] {
    if (drafting()) cancelDrawing();
    if (capturePending) finishFrame("Viewport resized.");
  });
  connect(view, &QGLView::rendererReplaced, this, &Controller::rendererChanged);
  connect(window->tabManager, &TabManager::tabCountChanged, this, &Controller::bindEditors);
  connect(window->tabManager, &TabManager::currentEditorChanged, this, [this] { emit changed(); });
  connect(window->tabManager, &TabManager::editorAboutToClose, this, [this](EditorInterface *editor) {
    if (editor == draftOwner) cancelDrawing();
    if (editor == displayedOwner) {
      displayedOwner.clear();
      result = {};
      refresh();
    }
    emit changed();
  });
  captureTimeout.setSingleShot(true);
  captureTimeout.setInterval(4000);
  connect(&captureTimeout, &QTimer::timeout, this,
          [this] { finishFrame("No viewport frame was presented before timeout."); });
  draftWatch.setInterval(250);
  connect(&draftWatch, &QTimer::timeout, this, [this] {
    if (drafting() && fileState() != draftFiles) cancelDrawing();
  });
  bindEditors();
  refresh();
  Bridge::instance()->registerController(this);
}
Controller::~Controller()
{
  view->composeAnnotations={};
  cancelDrawing();
  if (capturePending) finishFrame("Viewport closed.");
  delete palette;
  delete overlay;
}
void Controller::bindEditors()
{
  for (auto editor : window->tabManager->editorList) {
    if (editor->property("annotationControllerBound").toBool()) continue;
    editor->setProperty("annotationControllerBound", true);
    connect(SavedViews::forEditor(editor), &SavedViews::changed, this, &Controller::changed);
    connect(editor->annotations, &Store::sessionChanged, this, [this, editor] {
      if (draftOwner == editor) cancelDrawing();
      if (owner() == editor) {
        if (capturePending) finishFrame("Document was replaced.");
        displayedOwner.clear();
        result = {};
        refresh();
      }
      emit changed();
    });
    connect(editor->annotations, &Store::changed, this, [this, editor] {
      if (owner() == editor) refresh();
      emit changed();
    });
    connect(editor->annotations, &Store::changed, this,
            [this] { QTimer::singleShot(0, this, &Controller::updateActions); });
    connect(editor, &EditorInterface::contentsChanged, this, [this, editor] {
      if (drafting() && draftOwner == editor) cancelDrawing();
      emit changed();
    });
    connect(editor->parameterWidget, &ParameterWidget::parametersChanged, this, [this, editor] {
      if (drafting() && draftOwner == editor) cancelDrawing();
      emit changed();
    });
  }
}
EditorInterface *Controller::owner() const
{
  return displayedOwner;
}
QVector<EditorInterface *> Controller::editors() const
{
  QVector<EditorInterface *> result;
  for (auto editor : window->tabManager->editorList) result.append(editor);
  return result;
}
EditorInterface *Controller::actionOwner() const
{
  auto focus = QApplication::focusWidget();
  if (!focus) focus = window->focusWidget();
  if (focus &&
      (focus == view || view->isAncestorOf(focus) || focus == dock || dock->isAncestorOf(focus)))
    return owner();
  return nullptr;
}
bool Controller::busy() const
{
  return drafting() || capturePending || !evaluation.isEmpty() || GuiLocker::isLocked() ||
         (window->backgroundEvaluation && window->backgroundEvaluation->busy());
}
void Controller::updateActions()
{
  auto target = actionOwner();
  window->fileActionSave->setText(target ? tr("Save %1").arg(QFileInfo(label(target)).fileName())
                                         : originalSaveText);
  window->editActionUndo->setText(target ? tr("Undo in %1").arg(QFileInfo(label(target)).fileName())
                                         : originalUndoText);
  if (!target) target = window->activeEditor;
  window->editActionUndo->setEnabled(target && target->canUndo());
}
QJsonObject Controller::display() const
{
  return {{"axes", view->showAxes()},
          {"edges", view->showEdges()},
          {"crosshairs", view->showCrosshairs()},
          {"scale", view->showScaleProportional()},
          {"color_scheme", GlobalPreferences::inst()->getValue("3dview/colorscheme").toString()}};
}
void Controller::refresh()
{
  const QString selected =
    list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
  list->clear();
  ownerLabel->setText(
    owner() ? tr("Owner: %1%2")
                .arg(label(owner()), owner()->annotations->dirty() ? tr(" · annotations unsaved") : "")
            : tr("No annotation owner. Preview a document or select a blank-view owner."));
  if (!owner()) {
    showSelected();
    return;
  }
  if (!owner()->annotations->diagnostic().isEmpty())
    ownerLabel->setText(ownerLabel->text() + "\n" + owner()->annotations->diagnostic());
  for (auto it = owner()->annotations->records().crbegin();
       it != owner()->annotations->records().crend(); ++it) {
    QString comment = it->metadata.value("comment").toString();
    if (!comment.contains(search->text(), Qt::CaseInsensitive)) continue;
    auto item =
      new QListWidgetItem(comment.isEmpty() ? tr("(No comment)") : comment.simplified().left(110), list);
    item->setData(Qt::UserRole, it->id());
    item->setToolTip(it->metadata.value("created_utc").toString());
    if (it->id() == selected) list->setCurrentItem(item);
  }
  if (!list->currentItem() && list->count()) list->setCurrentRow(0);
  showSelected();
}
void Controller::showSelected()
{
  imageLabel->clear();
  commentLabel->clear();
  if (!owner() || !list->currentItem()) return;
  auto record = owner()->annotations->find(list->currentItem()->data(Qt::UserRole).toString());
  if (!record) return;
  QPixmap image;
  image.loadFromData(record->png, "PNG");
  imageLabel->setPixmap(image.scaled(qMax(1, imageLabel->width()), qMax(1, imageLabel->height()),
                                     Qt::KeepAspectRatio, Qt::SmoothTransformation));
  commentLabel->setText(record->metadata.value("comment").toString());
}
void Controller::setAccessWidget(QWidget *widget)
{
  panel->layout()->addWidget(widget);
}

QJsonObject Controller::camera() const
{
  const auto translation = view->cam.getVpt(), rotation = view->cam.getVpr();
  return {{"translation", QJsonArray{translation.x(), translation.y(), translation.z()}},
          {"rotation", QJsonArray{rotation.x(), rotation.y(), rotation.z()}},
          {"distance", view->cam.viewer_distance},
          {"fov", view->cam.fov},
          {"projection", view->orthoMode() ? "orthographic" : "perspective"}};
}
bool Controller::setCamera(const QJsonObject& value, QString *error)
{
  auto fail = [&](QString message) {
    if (error) *error = message;
    return false;
  };
  if (busy()) return fail("Viewport is busy.");
  if (!owner()) return fail("Viewport has no matching document.");
  auto translation = value.value("translation").toArray(), rotation = value.value("rotation").toArray();
  if (translation.size() != 3 || rotation.size() != 3)
    return fail("Camera vectors must have three finite numbers.");
  for (const auto& array : {translation, rotation})
    for (const auto& number : array)
      if (!number.isDouble() || !std::isfinite(number.toDouble()) || std::abs(number.toDouble()) > 1e12)
        return fail("Invalid camera vector.");
  const double distance = value.value("distance").toDouble(-1), fov = value.value("fov").toDouble(-1);
  const QString projection = value.value("projection").toString();
  if (!std::isfinite(distance) || distance <= 0 || distance > 1e12 || !std::isfinite(fov) || fov <= 0 ||
      fov >= 180 || (projection != "orthographic" && projection != "perspective"))
    return fail("Invalid camera distance, FOV or projection.");
  auto native = view->cam;
  native.setVpt(translation[0].toDouble(), translation[1].toDouble(), translation[2].toDouble());
  native.setVpr(rotation[0].toDouble(), rotation[1].toDouble(), rotation[2].toDouble());
  native.setVpd(distance);
  native.setVpf(fov);
  native.setProjection(projection == "orthographic" ? Camera::ProjectionType::ORTHOGONAL
                                                       : Camera::ProjectionType::PERSPECTIVE);
  if (!view->navigation()->permitted()) return fail("Native navigation is locked.");
  view->navigation()->cancel();
  view->navigation()->apply(CameraPose::from(native));
  emit changed();
  return true;
}
void trimHistory(QJsonArray& values, int count)
{
  while (values.size() > count || (!values.isEmpty() &&
         QJsonDocument(values).toJson(QJsonDocument::Compact).size() > 512 * 1024)) values.removeFirst();
}
QJsonValue customizerContext(EditorInterface *editor)
{
  const auto values = editor ? editor->parameterWidget->annotationValues() : QJsonObject{};
  if (QJsonDocument(values).toJson(QJsonDocument::Compact).size() > 65536) return QJsonValue::Null;
  return values;
}
QString Controller::viewRevision() const
{
  auto state = camera();
  state["width"] = view->width();
  state["height"] = view->height();
  state["dpr"] = view->devicePixelRatioF();
  if (state != observedView) { observedView = state; ++viewGeneration; }
  return QString::number(viewGeneration);
}
bool Controller::cameraOperation(const QJsonObject& operation, QString *error)
{
  if (busy() || QApplication::activeModalWidget()) { *error = "Viewport is busy."; return false; }
  if (!view->navigation()->permitted()) { *error = "Native navigation is locked."; return false; }
  const auto kind = operation.value("operation").toString("set");
  if (kind == "set") return setCamera(operation.value("camera").toObject(), error);
  if (kind == "projection") {
    auto value = camera();
    value["projection"] = operation.value("projection");
    return setCamera(value, error);
  }
  auto native = view->cam;
  if (kind == "fit") {
    if (view->getRenderer()) { native.autocenter = true; native.viewAll(view->getRenderer()->getBoundingBox()); }
  }
  else if (kind == "standard") {
    const auto name = operation.value("view").toString();
    if (name == "top") native.object_rot << 90, 0, 0;
    else if (name == "bottom") native.object_rot << 270, 0, 0;
    else if (name == "left") native.object_rot << 0, 0, 90;
    else if (name == "right") native.object_rot << 0, 0, 270;
    else if (name == "front") native.object_rot << 0, 0, 0;
    else if (name == "back") native.object_rot << 0, 0, 180;
    else if (name == "diagonal") native.object_rot << 35, 0, -25;
    else { *error = "Unknown standard view."; return false; }
  } else { *error = "Unknown camera operation."; return false; }
  view->navigation()->cancel();
  view->navigation()->apply(CameraPose::from(native));
  emit changed();
  return true;
}
QJsonObject Controller::savedViews() const
{
  auto editor = owner();
  auto store = editor ? editor->findChild<SavedViews *>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
  if (!store) return {{"available", false}, {"reason", "No displayed saved-view library."}};
  QJsonArray entries;
  for (const auto& entry : store->entries()) {
    Camera native = view->cam;
    entry.camera.apply(native);
    const auto rotation = native.getVpr(), translation = native.getVpt();
    entries.append(QJsonObject{{"id", entry.id}, {"name", entry.name}, {"camera", QJsonObject{
      {"rotation", QJsonArray{rotation.x(), rotation.y(), rotation.z()}},
      {"translation", QJsonArray{translation.x(), translation.y(), translation.z()}},
      {"distance", entry.camera.distance}, {"fov", entry.camera.fov},
      {"projection", entry.camera.projection == Camera::ProjectionType::ORTHOGONAL ? "orthographic" : "perspective"}}}});
  }
  return {{"available", true}, {"document_id", editor->annotations->sessionId}, {"window_id", windowId},
    {"viewport_id", viewportId}, {"library_revision", editor->annotations->sessionId + ":" + QString::number(store->revision())},
    {"dirty", store->dirty()}, {"problem", store->problem()}, {"views", entries}};
}
QJsonObject Controller::savedViewOperation(const QString& action, const QString& id,
                                          const QJsonObject& request, QString *error)
{
  const auto state = savedViews();
  if (!owner() || !state.value("available").toBool() || state.value("library_revision") != request.value("expected_library_revision")) {
    *error = "Saved-view library changed or is unavailable."; return {};
  }
  auto store = SavedViews::forEditor(owner());
  if (!store->problem().isEmpty()) { *error = store->problem(); return {}; }
  QString added;
  if (action == "add") {
    if (!view->navigation()->permitted()) { *error = "Native navigation is locked."; return {}; }
    view->navigation()->cancel(); // Freeze the displayed transition camera, never its destination.
    added = store->add(view->navigation()->current());
    if (added.isEmpty()) { *error = "Native saved-view limit or invalid camera."; return {}; }
  } else {
    const auto entry = store->find(id);
    if (!entry) { *error = "Saved view no longer exists."; return {}; }
    if (action == "rename") store->rename(id, request.value("name").toString());
    else if (action == "remove") store->remove(id);
    else if (action == "recall") {
      if (!view->navigation()->permitted()) { *error = "Native navigation is locked."; return {}; }
      const auto pose = entry->camera;
      view->navigation()->cancel();
      view->navigation()->apply(pose);
    } else { *error = "Unknown saved-view action."; return {}; }
  }
  auto response = savedViews();
  response.remove("views");
  response["views_route"] = "/v1/documents/" + owner()->annotations->sessionId + "/saved-views";
  response["viewport"] = status();
  if (!added.isEmpty()) response["added_id"] = added;
  emit changed();
  return response;
}

void Controller::finishEvaluation(bool success)
{
  if (evaluation.isEmpty()) {
    emit changed();
    return;
  }
  lastAttempt = evaluation;
  if (!lastAttempt.contains("duration_ms")) lastAttempt["duration_ms"] = double(evaluationTimer.elapsed());
  lastAttempt["outcome"] = success ? (lastAttempt.value("empty").toBool() ? "empty" : "installed") : "failed";
  if (success && window->evaluatedScene && evaluationOwner) {
    QJsonArray inputs;
    for (const auto& [key, input] : window->evaluatedScene->inputs->consumed()) {
      QJsonObject captured{{"file_key", QString::fromStdString(key)},
        {"source_sha256", QString::fromStdString(input.fingerprint)}};
      if (input.editorId) captured["document_id"] = QString::fromStdString(*input.editorId);
      if (input.editorRevision) captured["source_revision"] = QString::number(*input.editorRevision);
      inputs.append(captured);
    }
    lastAttempt["dependency_provenance"] = "captured";
    lastAttempt["evaluation_inputs"] = inputs;
    QJsonObject configuration{{"defines", QString::fromStdString(commandline_commands)},
      {"preview", window->isPreview}, {"backend", lastAttempt.value("backend")},
      {"time", lastAttempt.value("time")}, {"customizer", lastAttempt.value("customizer")},
      {"features", QString::fromStdString(Feature::features())}};
    const auto fingerprint = QString::fromLatin1(Store::hash(QJsonDocument(configuration).toJson(QJsonDocument::Compact)));
    lastAttempt["configuration_sha256"] = fingerprint;
    window->evaluationBinding->install(window->evaluatedScene, evaluationOwner,
      lastAttempt.value("attempt_id").toString(), fingerprint);
  } else window->evaluationBinding->invalidate(tr("Evaluation failed"));
  if (success) {
    lastAttempt["result_id"] = lastAttempt.value("attempt_id");
    // Detach while both root SourceFile and cached library ModuleInstantiations are alive.
    // Never recover mappings later from an old renderer's raw modinst pointers.
    try {
      auto tree = std::move(preparedInspection.value());
      tree.metadata["result_id"] = lastAttempt.value("result_id");
      tree.metadata["source"] = lastAttempt;
      if (Bridge::instance()->retainTree(tree)) {
        lastAttempt["tree_id"] = tree.metadata.value("tree_id");
        lastAttempt["tree_complete"] = tree.metadata.value("complete");
        lastAttempt["instantiated_node_count"] = tree.metadata.value("node_count");
        lastAttempt["dependencies"] = tree.metadata.value("dependencies");
      } else lastAttempt["tree_unavailable_reason"] = "Inspection retention budget exceeded.";
    } catch (const std::exception&) {
      lastAttempt["tree_unavailable_reason"] = "Inspection could not serialize this result.";
    }
  }
  const auto document = lastAttempt.value("document_id").toString();
  observeBackgroundAttempt(lastAttempt);
  if (success) {
    auto published = lastAttempt;
    published["result_id"] = lastAttempt.value("attempt_id");
    auto& results = resultHistory[document];
    results.append(published);
    trimHistory(results, 32);
  }
  {
    QMutexLocker lock(&contextMutex);
    messageContext = {};
  }
  if (success || !view->getRenderer() || installed.find(view->getRenderer()) == installed.end()) {
    displayedOwner = evaluationOwner;
    result = lastAttempt;
    if (view->getRenderer()) {
      result["renderer"] = QString::fromLatin1(typeid(*view->getRenderer()).name());
      installed[view->getRenderer()] = {view->renderer, displayedOwner, result};
    }
  }
  evaluation = {};
  evaluationOwner.clear();
  for (auto it = installed.begin(); it != installed.end();) {
    if (it->second.renderer.expired()) it = installed.erase(it);
    else ++it;
  }
  refresh();
  emit changed();
}
QJsonObject Controller::finishBackgroundEvaluation(EditorInterface *editor, QJsonObject metadata,
                                            InspectionTree inspection)
{
  evaluationOwner = editor;
  evaluation = std::move(metadata);
  evaluationTimer.start();
  preparedInspection = std::move(inspection);
  finishEvaluation(true);
  preparedInspection.reset();
  return lastAttempt;
}
void Controller::observeBackgroundAttempt(QJsonObject metadata)
{
  if (metadata.value("mode").toString() != "parameters" &&
      (metadata.value("outcome").toString() == "evaluating" ||
       lastAttempt.value("attempt_id") == metadata.value("attempt_id")))
    lastAttempt = metadata;
  auto& attempts = attemptHistory[metadata.value("document_id").toString()];
  const auto id = metadata.value("attempt_id");
  for (int i = attempts.size() - 1; i >= 0; --i)
    if (attempts[i].toObject().value("attempt_id") == id) attempts.removeAt(i);
  attempts.append(metadata);
  trimHistory(attempts, 64);
  emit changed();
}
QJsonObject Controller::diagnosticContext() const
{
  QMutexLocker lock(&contextMutex);
  return messageContext;
}
void Controller::observeDiagnostic(const Message& message, const QJsonObject& context)
{
  auto record = context;
  record["scope"] = context.isEmpty() ? "global" : "evaluation";
  record["message"] = QString::fromStdString(message.msg).left(8192);
  const auto group = message.group;
  record["severity"] = group == message_group::Error || group == message_group::Parser_Error || group == message_group::Export_Error || group == message_group::UI_Error ? "error" :
    group == message_group::Warning || group == message_group::UI_Warning || group == message_group::Font_Warning || group == message_group::Export_Warning || group == message_group::Deprecated ? "warning" :
    group == message_group::Echo ? "echo" : group == message_group::Trace ? "trace" : "info";
  // Native columns are not yet verified for every lexer path. Never label them UTF-8 spans.
  record["location"] = QJsonValue::Null;
  record["native_location"] = QJsonObject{{"path", QString::fromStdString(message.loc.fileName())},
    {"line", message.loc.firstLine()}, {"column", message.loc.firstColumn()}};
  record["truncated"] = message.msg.size() > 8192;
  auto& history = diagnosticHistory[context.value("document_id").toString()];
  history.append(record);
  trimHistory(history, 128);
  emit changed();
}
QJsonObject Controller::inspection(const QString& document) const
{
  QJsonArray attempts = attemptHistory.value(document);
  if (evaluation.value("document_id") == document) attempts.append(evaluation);
  QJsonObject value{{"attempts", attempts}, {"results", resultHistory.value(document)},
    {"diagnostics", diagnosticHistory.value(document)}, {"history_complete", false},
    {"history_policy", "observed_only_bounded"}};
  return value;
}
void Controller::pruneInspection()
{
  QSet<QString> live{QString()}; // Empty key is the bounded global-message history.
  for (auto editor : editors()) live.insert(editor->annotations->sessionId);
  for (auto history : {&attemptHistory, &resultHistory, &diagnosticHistory})
    for (auto it = history->begin(); it != history->end();)
      if (!live.contains(it.key())) it = history->erase(it);
      else ++it;
}
void Controller::rendererChanged()
{
  if (drafting()) cancelDrawing();
  if (capturePending) finishFrame("Renderer changed.");
  if (!evaluation.isEmpty()) return;
  auto it = installed.find(view->getRenderer());
  if (it != installed.end() && (!view->getRenderer() || !it->second.renderer.expired())) {
    displayedOwner = it->second.owner;
    result = it->second.result;
  } else result = {};
  refresh();
  emit changed();
}
void Controller::registerPresentationRenderer(const std::shared_ptr<Renderer>& renderer, const QString& resultId)
{
  if (result.value("result_id").toString() == resultId)
    installed[renderer.get()] = {renderer, displayedOwner, result};
}
QJsonObject Controller::status() const
{
  QJsonArray reasons;
  if (drafting()) reasons.append("annotation_draft");
  if (capturePending) reasons.append("capture");
  if (!evaluation.isEmpty() || GuiLocker::isLocked()) reasons.append("native_evaluation");
  if (window->backgroundEvaluation && window->backgroundEvaluation->busy()) reasons.append("background_evaluation");
  if (QApplication::activeModalWidget()) reasons.append("modal_interaction");
  QJsonObject value{
    {"window_id", windowId},
    {"viewport_id", viewportId},
    {"document_id", owner() ? owner()->annotations->sessionId : ""},
    {"active_document_id", window->activeEditor ? window->activeEditor->annotations->sessionId : ""},
    {"displayed_result_id", result.value("outcome") == "installed" || result.value("outcome") == "empty" ? result.value("result_id") : QJsonValue(QJsonValue::Null)},
    {"busy", busy() || QApplication::activeModalWidget()},
    {"busy_reasons", reasons},
    {"background_evaluations", window->backgroundEvaluation ? window->backgroundEvaluation->status() : QJsonArray{}},
    {"draft", drafting()},
    {"evaluation", evaluation.isEmpty() ? lastAttempt : evaluation},
    {"result", result},
    {"camera", camera()},
    {"view_revision", viewRevision()},
    {"capture_available", view->isVisible() && !window->isMinimized()},
    {"automatic_reload", window->designActionAutoReload->isChecked()},
    {"current_source_sha256", sourceHash(owner())},
    {"stale_source", owner() && result.value("source_sha256").toString() != sourceHash(owner())},
    {"stale_evaluation_inputs", !window->evaluationBinding->current()}};
  return value;
}
void Controller::startDrawing()
{
  if (busy() || !owner()) return;
  viewportTool=window->viewportTools->claimExternal("Annotations",this,[this](auto) { cancelDrawing(); });
  if (!viewportTool) return;
  dock->show();
  draftOwner = owner();
  draftResult = result;
  draftSize = view->size();
  draftDpr = view->devicePixelRatioF();
  draftSourceHash = sourceHash(draftOwner);
  drawing = {};
  lockedCamera = view->cam;
  ++draftToken;
  draftDisplay = display();
  draftFiles = fileState();
  draftWatch.start();
  reloadWasActive = window->autoReloadTimer->isActive();
  window->autoReloadTimer->stop();
  animationWasActive = window->animateWidget->animateTimer->isActive();
  animationInterval = window->animateWidget->animateTimer->interval();
  window->animateWidget->animateTimer->stop();
  GuiLocker::lock();
  state = DrawingState;
  overlay->setGeometry(view->rect());
  overlay->show();
  overlay->raise();
  palette->show();
  palette->raise();
  view->setFocus();
  view->setCursor(Qt::CrossCursor);
  emit changed();
}
void Controller::updateLock()
{
  if (drawing.marked() || state == Capturing || state == Comment)
    view->annotationCameraLock = lockedCamera;
  else view->annotationCameraLock.reset();
  overlay->update();
  view->update();
}
void Controller::cancelDrawing()
{
  if (!drafting()) return;
  if (window->viewportTools) window->viewportTools->finish(viewportTool);
  viewportTool=0;
  ++draftToken;
  draftWatch.stop();
  state = Idle;
  view->annotationCameraLock.reset();
  overlay->hide();
  palette->hide();
  view->unsetCursor();
  if (commentDialog) commentDialog->reject();
  drawing = {};
  draftOwner.clear();
  GuiLocker::unlock();
  if (reloadWasActive && window->designActionAutoReload->isChecked()) window->autoReloadTimer->start();
  if (animationWasActive) window->animateWidget->animateTimer->start(animationInterval);
  if (capturePending) finishFrame("Annotation draft was cancelled.");
  emit changed();
}
QByteArray Controller::fileState() const
{
  QJsonArray files;
  if (window->evaluatedScene) {
    for (const auto& [key, input] : window->evaluatedScene->inputs->consumed()) {
      QFileInfo info(QString::fromStdString(key));
      files.append(QJsonArray{QString::fromStdString(key), QString::number(info.size()),
                              QString::number(info.lastModified().toMSecsSinceEpoch())});
    }
  }
  return QJsonDocument(files).toJson(QJsonDocument::Compact);
}
bool Controller::eventFilter(QObject *object, QEvent *event)
{
  if (object == imageLabel && event->type() == QEvent::Resize) showSelected();
  if (!drafting() && actionOwner() &&
      (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
    auto key = static_cast<QKeyEvent *>(event);
    if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)) {
      event->accept();
      if (event->type() == QEvent::KeyPress)
        actionOwner()->undoFromAnnotations(key->matches(QKeySequence::Redo));
      return true;
    }
  }
  if (!drafting()) return QObject::eventFilter(object, event);
  if (state == DrawingState && !drawing.marked() &&
      (view->cam.object_trans != lockedCamera.object_trans ||
       view->cam.object_rot != lockedCamera.object_rot ||
       view->cam.viewer_distance != lockedCamera.viewer_distance || view->cam.fov != lockedCamera.fov ||
       view->cam.projection != lockedCamera.projection)) {
    drawing.navigation();
    lockedCamera = view->cam;
  }
  if (object == view && (event->type() == QEvent::Hide || event->type() == QEvent::Resize)) {
    cancelDrawing();
    return false;
  }
  if (state == Comment) return false;
  if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
    auto key = static_cast<QKeyEvent *>(event);
    const bool undo = key->matches(QKeySequence::Undo), redo = key->matches(QKeySequence::Redo);
    if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
        undo || redo) {
      event->accept();
      if (event->type() == QEvent::ShortcutOverride) return true;
      if (key->key() == Qt::Key_Escape) cancelDrawing();
      else if (state == DrawingState) {
        if (undo) drawing.undo();
        else if (redo) drawing.redo();
        else acceptDrawing();
        updateLock();
      }
      return true;
    }
  }
  if (object != view) return false;
  if (event->type() == QEvent::ContextMenu) return true;
  if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
      event->type() == QEvent::MouseMove) {
    auto mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() == Qt::RightButton) {
      cancelDrawing();
      return true;
    }
    if (state != DrawingState) return true;
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton &&
        mouse->modifiers() == Qt::NoModifier) {
      if (!drawing.marked()) lockedCamera = view->cam;
      drawing.begin(mouse->position(), view->size(), color);
      updateLock();
      return true;
    }
    if (drawing.pressed && event->type() == QEvent::MouseMove) {
      drawing.move(mouse->position(), view->size());
      updateLock();
      return true;
    }
    if (mouse->button() == Qt::LeftButton && event->type() == QEvent::MouseButtonRelease) {
      drawing.end(view->size());
      updateLock();
      return true;
    }
    if (drawing.marked()) return true;
  }
  if (event->type() == QEvent::Wheel && drawing.marked()) return true;
  return false;
}
void Controller::capture(const QString& expectedResult,
                         std::function<void(QJsonObject, QByteArray, QString)> done)
{
  if (busy()) {
    done({}, {}, "Viewport is busy.");
    return;
  }
  requestFrame(false, expectedResult, std::move(done));
}
void Controller::requestFrame(bool annotation, const QString& expectedResult,
                              std::function<void(QJsonObject, QByteArray, QString)> done)
{
  if(window->evaluationBinding && window->evaluationBinding->draftPixels()){
    done({}, {}, "Draft geometry is unavailable for live result capture.");return;
  }
  if (!owner() || capturePending || !view->isVisible() || window->isMinimized() ||
      QApplication::activeModalWidget() ||
      (!expectedResult.isEmpty() && result.value("result_id").toString() != expectedResult)) {
    done({}, {}, "Missing owner or stale result.");
    return;
  }
  capturePending = true;
  view->navigation()->cancel();
  captureDone = std::move(done);
  const auto token = ++captureToken;
  QPointer<EditorInterface> target = owner();
  const auto session = target->annotations->sessionId;
  const auto boundResult = result, boundCamera = camera();
  const auto boundView = viewRevision();
  const auto size = view->size();
  const auto dpr = view->devicePixelRatioF();
  frameConnection = connect(
    view, &QGLView::frameRendered, this,
    [this, annotation, token, target, session, boundResult, boundCamera, boundView, size, dpr] {
      if (!capturePending || token != captureToken) return;
      disconnect(frameConnection);
      if (!target || target != owner() || target->annotations->sessionId != session ||
          result != boundResult || camera() != boundCamera || view->size() != size ||
          view->devicePixelRatioF() != dpr || viewRevision() != boundView ||
          (!annotation && (GuiLocker::isLocked() || QApplication::activeModalWidget()))) {
        finishFrame("Capture target changed.");
        return;
      }
      QImage image = view->grabFrame(QGLView::CaptureContent::ModelOnly).copy();
      image.setDevicePixelRatio(1);
      if (image.isNull() || qint64(image.width()) * image.height() > 64000000) {
        finishFrame("Invalid/oversized viewport framebuffer.");
        return;
      }
      if (annotation) {
        QPainter painter(&image);
        drawing.paint(painter, image.size(), dpr);
      }
      QJsonArray modelview, projection;
      for (int i = 0; i < 16; ++i) {
        modelview.append(view->modelview[i]);
        projection.append(view->projection[i]);
      }
      QJsonObject metadata{
        {"id", uuid()},
        {"created_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"comment", ""},
        {"width", image.width()},
        {"height", image.height()},
        {"dpr", dpr},
        {"camera", boundCamera},
        {"view_revision", boundView},
        {"displayed_result_id", boundResult.value("result_id")},
        {"capture_policy", "model_only_without_guides_statistics_or_controls"},
        {"modelview", modelview},
        {"projection_matrix", projection},
        {"result", boundResult},
        {"document_id", target->annotations->sessionId},
        {"path", target->filepath},
        {"current_source_sha256", sourceHash(target)},
        {"stale_source", sourceHash(target) != boundResult.value("source_sha256").toString()},
        {"strokes", annotation ? drawing.json() : QJsonArray{}},
        {"display", display()}};
      auto watcher = new QFutureWatcher<QPair<QByteArray, QByteArray>>(this);
      connect(watcher, &QFutureWatcher<QPair<QByteArray, QByteArray>>::finished, this,
        [this, watcher, metadata, token]() mutable {
          const auto encoded = watcher->result();
          watcher->deleteLater();
          if (!capturePending || token != captureToken) return;
          if (encoded.first.isEmpty()) { finishFrame("Cannot encode viewport PNG within limits."); return; }
          metadata["png_sha256"] = QString::fromLatin1(encoded.second);
          captureTimeout.stop();
          capturePending = false;
          auto done = std::move(captureDone);
          captureDone = {};
          done(metadata, encoded.first, {});
          emit changed();
        });
      watcher->setFuture(QtConcurrent::run([image = std::move(image)] {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG") || png.size() > Store::MaxPng) png.clear();
        return qMakePair(png, Store::hash(png));
      }));
    },
    Qt::QueuedConnection);
  captureTimeout.start();
  view->update();
  emit changed();
}
void Controller::finishFrame(const QString& error)
{
  if (!capturePending) return;
  disconnect(frameConnection);
  captureTimeout.stop();
  capturePending = false;
  ++captureToken;
  auto done = std::move(captureDone);
  captureDone = {};
  if (done) done({}, {}, error);
  emit changed();
}
void Controller::acceptDrawing()
{
  if (state != DrawingState || drawing.pressed) return;
  if (!draftOwner || draftOwner != owner() || draftResult != result || draftSize != view->size() ||
      draftDpr != view->devicePixelRatioF() || draftSourceHash != sourceHash(draftOwner) ||
      display() != draftDisplay) {
    cancelDrawing();
    return;
  }
  if (drawing.marked()) view->cam = lockedCamera;
  else lockedCamera = view->cam;
  state = Capturing;
  updateLock();
  const auto token = draftToken;
  requestFrame(true, result.value("result_id").toString(),
               [this, token](QJsonObject metadata, QByteArray png, QString error) {
                 if (token != draftToken || !draftOwner) return;
                 if (!error.isEmpty()) {
                   cancelDrawing();
                   QMessageBox::warning(window, tr("Annotation capture"), error);
                   return;
                 }
                 state = Comment;
                 QDialog dialog(window);
                 commentDialog = &dialog;
                 dialog.setWindowTitle(tr("Add annotation"));
                 QVBoxLayout layout(&dialog);
                 layout.addWidget(new QLabel(
                   tr("Optional comment. The screenshot is kept exactly as captured."), &dialog));
                 QPlainTextEdit text(&dialog);
                 text.setObjectName("annotationComment");
                 layout.addWidget(&text);
                 QDialogButtonBox buttons(&dialog);
                 auto add = buttons.addButton(tr("Add"), QDialogButtonBox::AcceptRole);
                 auto back = buttons.addButton(tr("Back"), QDialogButtonBox::ActionRole);
                 buttons.addButton(QDialogButtonBox::Cancel);
                 layout.addWidget(&buttons);
                 connect(add, &QPushButton::clicked, &dialog, &QDialog::accept);
                 connect(back, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(2); });
                 connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                 text.setFocus();
                 dialog.resize(440, 260);
                 const int answer = dialog.exec();
                 commentDialog.clear();
                 if (token != draftToken || !draftOwner) return;
                 if (answer == 2) {
                   state = DrawingState;
                   updateLock();
                   view->setFocus();
                   return;
                 }
                 if (answer == QDialog::Accepted) {
                   metadata["comment"] = text.toPlainText();
                   if (!draftOwner->annotations->add({metadata, png}, &error))
                     QMessageBox::warning(window, tr("Annotation not added"), error);
                 }
                 cancelDrawing();
                 panel->setFocus();
               });
}
}  // namespace AgentAnnotations
