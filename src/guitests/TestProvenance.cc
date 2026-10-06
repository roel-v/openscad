#include "TestProvenance.h"
#include "gui/selection/ProvenanceService.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/selection/InspectorDock.h"
#include "gui/ScintillaEditor.h"
#include "gui/CameraNavigation.h"
#include "gui/QGLView.h"
#include "glview/RenderSettings.h"
#include <QTest>
#include <QSignalSpy>
#include <QFile>
#include <QTemporaryDir>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>
#include <QMessageBox>
#include <QApplication>
#include <QAbstractButton>

using namespace SourceProvenance;
namespace {
bool write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
const Instance *ancestor(const Resolved& resolved, const std::string& name) {
  if (!resolved.result) return nullptr;
  for (const auto candidate : resolved.mapping.candidateInstances) {
    auto instance = resolved.result->instance(candidate);
    while (instance) {
      if (instance->call.callable == name) return instance;
      instance = instance->parent ? resolved.result->instance(*instance->parent) : nullptr;
    }
  }
  return nullptr;
}
void discardDialogs(QTimer& timer) {
  QObject::connect(&timer, &QTimer::timeout, [] {
    if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      if (box->standardButtons() == (QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel))
        box->button(QMessageBox::Discard)->click();
  });
  timer.start(10);
}
}
void TestProvenance::initTestCase() {
  window->hide(); window->show(); window->raise(); window->activateWindow();
  QVERIFY(QTest::qWaitForWindowExposed(window));
  QTRY_VERIFY(window->qglview->isValid());
}
void TestProvenance::init() {
  for (auto editor : window->tabManager->editorList) editor->setContentModified(false);
  QTimer discard; discardDialogs(discard); restoreWindowInitialState();
  window->show(); window->raise(); window->activateWindow();
  window->designActionAutoReload->setChecked(false);
  previousBackend = static_cast<int>(RenderSettings::inst()->backend3D);
  RenderSettings::inst()->backend3D = RenderBackend3D::ManifoldBackend;
  window->qglview->navigation()->settings.animate = false;
  window->viewActionThrownTogether->setChecked(false);
  window->viewActionPreview->setChecked(true);
}
void TestProvenance::cleanup() {
  RenderSettings::inst()->backend3D = static_cast<RenderBackend3D>(previousBackend);
  for (auto editor : window->tabManager->editorList) {
    if (auto scintilla = qobject_cast<ScintillaEditor *>(editor)) scintilla->qsci->setReadOnly(false);
    editor->setContentModified(false);
  }
  QTimer discard; discardDialogs(discard); restoreWindowInitialState();
  window->designActionAutoReload->setChecked(false);
}
bool TestProvenance::compile(const QString& source, bool final) {
  window->activeEditor->setPlainText(source);
  QSignalSpy completed(window, &MainWindow::compilationDone);
  if (final) window->designActionRender->trigger(); else window->actionRenderPreview();
  if (completed.empty() && !completed.wait(15000)) return false;
  return window->evaluationBinding->current();
}
std::optional<Selection::Hit> TestProvenance::pick(const Vector3d& point) {
  auto view = window->qglview;
  view->navigation()->apply({CameraPose::upright({0,-1,0}), -point, 80, 45, Camera::ProjectionType::ORTHOGONAL});
  view->update(); QTest::qWait(20);
  view->makeCurrent(); view->setupCamera();
  const Eigen::Map<const Eigen::Matrix4d> modelview(view->modelview), projection(view->projection);
  Eigen::Vector4d projected = projection*modelview*Eigen::Vector4d(point.x(),point.y(),point.z(),1);
  projected /= projected.w(); view->doneCurrent();
  const QPointF pixel{(projected.x()+1)*view->cam.pixel_width/(2*view->devicePixelRatio()),
                      (1-projected.y())*view->cam.pixel_height/(2*view->devicePixelRatio())};
  return window->selectionService->pick(pixel, window->evaluationBinding->stamp()).hit;
}

void TestProvenance::previewInstances() {
  QVERIFY(compile("module part(size=4){scale([2,1,0.5]) cube(size,center=true);} translate([-8,0,0]) part(); translate([8,0,0]) part();"));
  auto firstHit = pick({-8,-2,0}), secondHit = pick({8,-2,0}); QVERIFY(firstHit && secondHit);
  auto service = ProvenanceService::forWindow(window);
  auto first = service->resolve(*firstHit), second = service->resolve(*secondHit);
  QCOMPARE(first.mapping.confidence, Confidence::Unique); QCOMPARE(second.mapping.confidence, Confidence::Unique);
  QVERIFY(first.mapping.candidateInstances != second.mapping.candidateInstances);
  const auto a = ancestor(first,"part"), b = ancestor(second,"part"); QVERIFY(a && b && a->definition && b->definition);
  QVERIFY(a->definition->syntax == b->definition->syntax); QVERIFY(!(a->call.syntax == b->call.syntax));
  QVERIFY(first.mapping.localToWorld->matrix().isApprox(firstHit->localToWorld->matrix(),1e-10));
  QVERIFY(second.mapping.localToWorld->translation().isApprox(Vector3d(8,0,0)));
  QVERIFY(service->navigate(second,b->id,NavigationSite::Call));
  QVERIFY(window->activeEditor->selectedText().startsWith("part("));
  QVERIFY(service->navigate(second,b->id,NavigationSite::Definition));
  QVERIFY(window->activeEditor->selectedText().startsWith("module part("));
  auto forged = *secondHit; forged.localToWorld->translate(Vector3d(1,0,0));
  QCOMPARE(service->resolve(forged).mapping.confidence,Confidence::Unavailable);
  window->inspectorDock->show(); window->inspectorDock->raise(); window->inspectorDock->refresh();
  auto label = window->inspectorDock->findChild<QLabel *>("sourceProvenanceStatus"); QVERIFY(label);
  QVERIFY(label->text().contains("one producing invocation"));
  auto combo = window->inspectorDock->findChild<QComboBox *>("provenanceCandidates"); QVERIFY(combo);
  const auto partIndex = combo->findData(QVariant::fromValue<qulonglong>(b->id)); QVERIFY(partIndex > 0);
  combo->setCurrentIndex(partIndex);
  QVERIFY(service->navigate(second,b->id,NavigationSite::Call));
  auto definitionButton = window->inspectorDock->findChild<QPushButton *>("provenanceGoToDefinition"); QVERIFY(definitionButton && definitionButton->isEnabled());
  QTest::mouseClick(definitionButton,Qt::LeftButton);
  QTRY_VERIFY(window->activeEditor->selectedText().startsWith("module part("));
  const auto output = qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");
  if (!output.isEmpty()) { QTest::qWait(20); QVERIFY(window->grab().save(output+"/source-provenance.png")); }
}

void TestProvenance::finalBoolean() {
  const QString source = "difference(){cube(10,center=true); translate([0,-4,0]) cube([4,6,4],center=true);}";
  QVERIFY(compile(source)); auto preview = pick({0,-1,0}); QVERIFY(preview);
  auto boundary = ProvenanceService::forWindow(window)->resolve(*preview);
  QCOMPARE(boundary.mapping.confidence,Confidence::Unique); QCOMPARE(boundary.mapping.boundaryPolarity,-1);
  for (auto backend : {RenderBackend3D::ManifoldBackend,RenderBackend3D::CGALBackend}) {
    RenderSettings::inst()->backend3D = backend;
    QVERIFY(compile(source,true)); auto hit = pick({0,-1,0}); QVERIFY(hit);
    auto resolved = ProvenanceService::forWindow(window)->resolve(*hit);
    QCOMPARE(resolved.mapping.confidence,Confidence::Multiple); QVERIFY(resolved.mapping.candidateInstances.size() >= 2);
    QVERIFY(!resolved.mapping.localToWorld);
    const auto instance = resolved.result->instance(resolved.mapping.candidateInstances.front());
    QVERIFY(!ProvenanceService::forWindow(window)->editableSpan(resolved,instance->id,instance->call.arguments[0].expression,"8"));
    auto combo = window->inspectorDock->findChild<QComboBox *>("provenanceCandidates"); QVERIFY(combo); QCOMPARE(combo->currentIndex(),0);
  }
}

void TestProvenance::sourceTransactions() {
  const QString source = "//é\r\nwidth=20; cube(size = width/2, center=true);";
  QVERIFY(compile(source)); auto hit = pick({0,-5,0}); QVERIFY(hit);
  auto service = ProvenanceService::forWindow(window); auto resolved = service->resolve(*hit);
  const auto instance = ancestor(resolved,"cube"); QVERIFY(instance);
  const auto& expression = instance->call.arguments[0].expression;
  QCOMPARE(instance->call.arguments[0].originalExpression,std::string("width/2"));
  auto editor = qobject_cast<ScintillaEditor *>(window->activeEditor); QVERIFY(editor);
  editor->qsci->setReadOnly(true); QVERIFY(!service->editableSpan(resolved,instance->id,expression,"12"));
  editor->qsci->setReadOnly(false);
  auto target = service->editableSpan(resolved,instance->id,expression,"12"); QVERIFY(target); QVERIFY(service->validateTarget(*target));
  QCOMPARE(editor->toPlainText(),source); // Preparing a target never writes.
  auto applied = SourceEditing::apply(*target->editor,target->request); QCOMPARE(applied.error,SourceEditing::EditError::None); QVERIFY(applied.changed);
  QCOMPARE(editor->toPlainText(),QString("//é\r\nwidth=20; cube(size = 12, center=true);"));
  QVERIFY(!service->validateTarget(*target));
  editor->undo(); QCOMPARE(editor->toPlainText(),source); QCOMPARE(service->resolve(*hit).mapping.confidence,Confidence::Stale);
  editor->redo(); QVERIFY(editor->toPlainText().contains("size = 12"));
}

void TestProvenance::dirtyDependencyNavigation() {
  QTemporaryDir directory; QVERIFY(directory.isValid());
  const auto root = directory.filePath("root.scad"), library = directory.filePath("library.scad"), values = directory.filePath("values.scad");
  QVERIFY(write(root,"include <values.scad>\nuse <library.scad>\npart(width);"));
  QVERIFY(write(library,"module part(size=2){cube(size,center=true);}")); QVERIFY(write(values,"width=2;"));
  window->tabManager->open(library); QPointer<EditorInterface> libraryEditor = window->activeEditor;
  libraryEditor->setPlainText("module part(size = 8/2){cube(size,center=true);}");
  window->tabManager->open(values); QPointer<EditorInterface> valuesEditor = window->activeEditor; valuesEditor->setPlainText("//é\r\nwidth=6;");
  window->tabManager->open(root); QPointer<EditorInterface> rootEditor = window->activeEditor;
  QVERIFY(compile(rootEditor->toPlainText())); auto hit = pick({0,-3,0}); QVERIFY(hit);
  auto service = ProvenanceService::forWindow(window); auto resolved = service->resolve(*hit); const auto part = ancestor(resolved,"part"); QVERIFY(part && part->definition);
  QCOMPARE(part->definition->arguments[0].originalExpression,std::string("8/2"));
  QVERIFY(service->navigate(resolved,part->id,NavigationSite::Definition)); QCOMPARE(window->activeEditor,libraryEditor.data());
  QVERIFY(libraryEditor->selectedText().startsWith("module part("));
  window->tabManager->open(root); QVERIFY(service->navigate(resolved,part->id,NavigationSite::Call));
  QCOMPARE(window->activeEditor,rootEditor.data()); QCOMPARE(rootEditor->selectedText(),QString("part(width)"));
  valuesEditor->setPlainText("width=7;"); QCOMPARE(service->resolve(*hit).mapping.confidence,Confidence::Stale);
  QVERIFY(!service->navigate(resolved,part->id,NavigationSite::Definition));
}

void TestProvenance::staleRecompileAndFailure() {
  QVERIFY(compile("cube(10,center=true);")); auto original = pick({0,-5,0}); QVERIFY(original);
  auto service = ProvenanceService::forWindow(window); auto resolved = service->resolve(*original);
  const auto instance = ancestor(resolved,"cube"); QVERIFY(instance);
  QVERIFY(compile("//inserted before call\ncube(10,center=true);"));
  QCOMPARE(service->resolve(*original).mapping.confidence,Confidence::Stale);
  auto changed = pick({0,-5,0}); QVERIFY(changed); QCOMPARE(changed->object.nodeIndex,original->object.nodeIndex);
  QVERIFY(!service->editableSpan(resolved,instance->id,instance->call.arguments[0].expression,"12"));
  window->activeEditor->setPlainText("cube("); window->actionRenderPreview();
  QVERIFY(!window->evaluationBinding->current()); QCOMPARE(service->resolve(*changed).mapping.confidence,Confidence::Stale);
  QVERIFY(compile("cube(10,center=true);")); QCOMPARE(service->resolve(*changed).mapping.confidence,Confidence::Stale);
}

void TestProvenance::navigationTabRace() {
  QTemporaryDir directory; QVERIFY(directory.isValid());
  const auto root = directory.filePath("root.scad"), library = directory.filePath("library.scad"), other = directory.filePath("other.scad");
  QVERIFY(write(root,"use <library.scad> part();")); QVERIFY(write(library,"module part(){cube(10,center=true);}")); QVERIFY(write(other,"sphere(2);"));
  window->tabManager->open(other); QPointer<EditorInterface> otherEditor = window->activeEditor;
  window->tabManager->open(root); QVERIFY(compile(window->activeEditor->toPlainText()));
  auto hit = pick({0,-5,0}); QVERIFY(hit); auto service = ProvenanceService::forWindow(window); auto resolved = service->resolve(*hit); auto part = ancestor(resolved,"part"); QVERIFY(part && part->definition);
  QVERIFY(!service->editableSpan(resolved,part->id,part->definition->whole,"module part(){}")); // Disk-only input.
  bool redirected = false;
  auto connection = connect(window->tabManager,&TabManager::currentEditorChanged,this,[&](EditorInterface *editor) {
    if (!redirected && editor && editor->filepath == library) { redirected = true; window->tabManager->open(other); }
  });
  QVERIFY(!service->navigate(resolved,part->id,NavigationSite::Definition)); disconnect(connection);
  QVERIFY(redirected); QCOMPARE(window->activeEditor,otherEditor.data()); QCOMPARE(otherEditor->toPlainText(),QString("sphere(2);")); QVERIFY(otherEditor->selectedText().isEmpty());
}

void TestProvenance::closedSource() {
  window->tabManager->actionNew(); QPointer<EditorInterface> editor = window->activeEditor;
  QVERIFY(compile("cube(10,center=true);")); auto hit = pick({0,-5,0}); QVERIFY(hit);
  auto service = ProvenanceService::forWindow(window); auto resolved = service->resolve(*hit); const auto instance = ancestor(resolved,"cube"); QVERIFY(instance);
  auto target = service->editableSpan(resolved,instance->id,instance->call.arguments[0].expression,"12"); QVERIFY(target);
  editor->setContentModified(false); window->tabManager->closeCurrentTab(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  QVERIFY(!editor); QVERIFY(!target->editor); QVERIFY(!service->validateTarget(*target));
  QCOMPARE(service->resolve(*hit).mapping.confidence,Confidence::Stale); QVERIFY(!service->navigate(resolved,instance->id,NavigationSite::Call));
}
