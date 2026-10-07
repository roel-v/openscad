#include "TestSemanticHierarchy.h"
#include "gui/semantic/SemanticController.h"
#include "gui/semantic/SemanticExportJob.h"
#include "gui/semantic/SemanticParameterPanel.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/selection/InspectorDock.h"
#include "gui/selection/ProvenanceService.h"
#include "gui/design/DesignDocument.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/ScintillaEditor.h"
#include "core/NativeExecutionGuard.h"
#include "core/semantic/SemanticPlacement.h"
#include "glview/RenderSettings.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QPushButton>
#include <QTreeView>
#include <QTest>
#include <QApplication>
#include <QFile>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QTabWidget>
#include <QBuffer>
#include <QDateTime>
#include <QJsonArray>
#include <QCheckBox>
#include <QLineEdit>
#include <QInputMethodEvent>
#include <QToolButton>
#include <QMenu>
#include <QTimer>
#include <QScopeGuard>
#include <QInputDialog>
#include "gui/parameter/ParameterVirtualWidget.h"
#include <QUuid>
#include <future>
namespace {
const QString part="@{type:Part,id:\"bracket\",label:\"Bracket\"} module bracket(width=30){difference(){cube([width,10,3]);translate([12,-1,-1]) cube([6,7,5]);}}\n";
const QString pairDefinition=part+"@{type:Assembly,id:\"pair-def\"} module pair(width=35,gap=60){@{type:Component,id:\"left\"} bracket(width);translate([gap,0,0]) @{type:Component,id:\"right\"} bracket(width+10);}\n";
const QString pair=pairDefinition+"@{type:Component,id:\"pair\"} pair();";
QPointF projectedPixel(QGLView& view,const Vector3d& world){
  view.makeCurrent();view.setupCamera();const Eigen::Map<const Eigen::Matrix4d> model(view.modelview),projection(view.projection);
  Vector4d clip=projection*model*Vector4d(world.x(),world.y(),world.z(),1);clip/=clip.w();view.doneCurrent();
  return {((clip.x()+1)*view.cam.pixel_width/2)/view.devicePixelRatio(),((1-clip.y())*view.cam.pixel_height/2)/view.devicePixelRatio()};
}
Selection::PickResult pick(MainWindow *window,Vector3d point,Vector3d normal){
  auto *view=window->qglview;view->navigation()->apply({CameraPose::upright(normal),-point,100,45,Camera::ProjectionType::ORTHOGONAL});
  view->update();QTest::qWait(20);return window->selectionService->pick(projectedPixel(*view,point),window->evaluationBinding->stamp());
}
struct EffectProbe {int colors=0,picks=0;};
AgentAnnotations::Record parameterAnnotation(){
  QImage image(8,8,QImage::Format_RGB32);image.fill(Qt::blue);QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");
  return {{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"comment","semantic parameter undo"},{"created_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    {"width",8},{"height",8},{"png_sha256",QString::fromLatin1(AgentAnnotations::Store::hash(png))},
    {"camera",QJsonObject{{"translation",QJsonArray{0,0,0}},{"rotation",QJsonArray{0,0,0}},{"distance",100},{"fov",45},{"projection","perspective"}}},{"strokes",QJsonArray{}},{"result",QJsonObject{}}},png};
}
class IdentityEffect : public Renderer {
public:
  IdentityEffect(std::shared_ptr<Renderer> child,std::shared_ptr<EffectProbe> probe):child(std::move(child)),probe(std::move(probe)){}
  void prepare(const ShaderUtils::ShaderInfo *shader) override {child->prepare(shader);}
  void draw(bool edges,const ShaderUtils::ShaderInfo *shader) const override {if(shader && shader->type==ShaderUtils::ShaderType::SURFACE_PICK)++probe->picks;else ++probe->colors;child->draw(edges,shader);}
  BoundingBox getBoundingBox() const override{return child->getBoundingBox();}
  SurfaceContext prepareSurfacePick(const ShaderUtils::ShaderInfo *shader) override{return child->prepareSurfacePick(shader);}
private:std::shared_ptr<Renderer> child;std::shared_ptr<EffectProbe> probe;
};
}
void TestSemanticHierarchy::foreignPresentationDrawIdsRemainUnowned(){
  evaluate(part+"@{type:Component,id:\"a\"}bracket();");if(QTest::currentTestFailed())return;
  auto presentation=window->inspectionPresentation;QTRY_COMPARE(presentation->status(),InspectionPresentationController::Status::Ready);
  auto semantic=window->semanticController;QVERIFY(semantic->select({Semantic::TargetRole::Instance,"/a"}));
  const auto source=window->activeEditor->toPlainText();window->activeEditor->clearUndoHistory();
  auto foreign=std::make_shared<InspectionMeshSet>(*presentation->meshes());
  QVERIFY(!foreign->pieces.empty());for(auto& piece:foreign->pieces)piece.drawId+=1000000;
  foreign->geometryGeneration+=1000000;QObject lifetime;
  auto provider=presentation->registerMeshProvider("foreign semantic draw IDs",500,&lifetime,[foreign](auto,auto,auto done){
    done({InspectionPresentationController::Status::Ready,foreign,{}});return InspectionPresentationController::Cancel{};
  });
  auto request=presentation->requestMeshes("foreign semantic fixture",&lifetime);
  QTRY_COMPARE(presentation->meshes(),std::shared_ptr<const InspectionMeshSet>(foreign));
  QVERIFY(semantic->select({Semantic::TargetRole::Instance,"/a"}));
  QVERIFY(!semantic->pathForDraw(foreign->pieces.front().drawId));
  QVERIFY(!window->selectionService->state().object);
  QCOMPARE(window->activeEditor->toPlainText(),source);QVERIFY(!window->activeEditor->canUndo());
  request.reset();provider.reset();QTRY_COMPARE(presentation->status(),InspectionPresentationController::Status::Ready);
  QVERIFY(semantic->select({Semantic::TargetRole::Instance,"/a"}));QVERIFY(window->selectionService->state().object);
}
void TestSemanticHierarchy::initTestCase(){
  window->hide();window->show();window->raise();window->activateWindow();QVERIFY(QTest::qWaitForWindowExposed(window));QTRY_VERIFY(window->qglview->isValid());
  auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");if(!output.isEmpty()){window->qglview->makeCurrent();auto info=window->qglview->getRendererInfo();window->qglview->doneCurrent();QFile file(output+"/renderer-info.txt");QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write(QByteArray::fromStdString(info))>0);}
}
void TestSemanticHierarchy::init(){
  for(auto *editor:window->tabManager->editorList)editor->setContentModified(false);restoreWindowInitialState();window->designActionAutoReload->setChecked(false);
  previousBackend=static_cast<int>(RenderSettings::inst()->backend3D);RenderSettings::inst()->backend3D=RenderBackend3D::ManifoldBackend;
  window->qglview->navigation()->settings.animate=false;window->show();window->raise();window->activateWindow();
  if(auto *panel=window->semanticController->parameterPanel())if(auto *mode=panel->findChild<QComboBox *>("semanticParameterMode"))mode->setCurrentIndex(0);
}
void TestSemanticHierarchy::cleanup(){
  RenderSettings::inst()->backend3D=static_cast<RenderBackend3D>(previousBackend);
  for(auto *editor:window->tabManager->editorList)editor->setContentModified(false);restoreWindowInitialState();window->designActionAutoReload->setChecked(false);
}
void TestSemanticHierarchy::evaluate(const QString& source,bool final){
  window->activeEditor->setPlainText(source);QSignalSpy done(window,&MainWindow::compilationDone);
  if(final)window->designActionRender->trigger();else window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),15000);
  QTRY_VERIFY_WITH_TIMEOUT(window->evaluationBinding->current(),5000);
  QTRY_COMPARE_WITH_TIMEOUT(window->inspectionPresentation->status(),InspectionPresentationController::Status::Ready,15000);
}
void TestSemanticHierarchy::hierarchyAndTargets(){
  evaluate(pair);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;auto *model=controller->model();
  QVERIFY(model->find(Semantic::TreeRole::Definition,"bracket").isValid());auto right=model->find(Semantic::TreeRole::Instance,"/pair/right");QVERIFY(right.isValid());QCOMPARE(model->parent(right),model->find(Semantic::TreeRole::Instance,"/pair"));
  QVERIFY(controller->select({Semantic::TargetRole::Instance,"/pair/right"}));auto *service=controller->targetService();
  auto key=Semantic::TargetKey{Semantic::TargetRole::Instance,"/pair/right"};QTRY_VERIFY(service->resolve(key).state==Design::ResolutionState::Resolved);
  auto target=service->resolve(key);QCOMPARE(target.localBounds->max().x(),45.0);QCOMPARE(target.worldBounds->max().x(),105.0);
  QVERIFY(controller->select({Semantic::TargetRole::Definition,"bracket"}));key={Semantic::TargetRole::Definition,"bracket"};QTRY_VERIFY(service->resolve(key).state==Design::ResolutionState::Resolved);QCOMPARE(service->resolve(key).localBounds->max().x(),30.0);
  QVERIFY(controller->select({Semantic::TargetRole::Instance,"/pair"}));key={Semantic::TargetRole::Instance,"/pair"};QTRY_VERIFY(service->resolve(key).state==Design::ResolutionState::Resolved);QCOMPARE(service->resolve(key).localBounds->max().x(),105.0);
}
void TestSemanticHierarchy::displayRefreshKeepsStrictEditGuards(){
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto dependency=directory.filePath("width.scad");
  auto write=[&](const QByteArray& bytes){QFile file(dependency);return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();};
  QVERIFY(write("width=30;\n"));
  evaluate("include <"+dependency+">\n"+part+"@{type:Component,id:\"a\"} bracket(width);\n");
  if(QTest::currentTestFailed())return;
  auto *controller=window->semanticController;auto *service=controller->targetService();
  const Semantic::TargetKey key{Semantic::TargetRole::Instance,"/a"};
  QVERIFY(controller->select(key));
  QTRY_VERIFY(service->resolve(key).state==Design::ResolutionState::Resolved);
  auto *binding=window->evaluationBinding;const auto stamp=binding->stamp();
  const auto captured=binding->scene(stamp);QVERIFY(captured!=nullptr);
  const auto source=window->activeEditor->toPlainText();
  QVERIFY(write("width=40;\n"));
  // No watcher delivery: repeated read-only panel refreshes retain the display.
  for(int i=0;i<20;++i){
    controller->parameterPanel()->refresh();window->inspectorDock->refresh();
    QVERIFY(service->displayCurrent(stamp));
    QCOMPARE(service->resolveForDisplay(key).state,Design::ResolutionState::Resolved);
    QVERIFY(controller->selectedTarget()==std::optional<Semantic::TargetKey>(key));
  }
  // Actions still reject changed disk contents before any event-loop turn.
  QVERIFY(!service->validate(stamp));
  QCOMPARE(service->resolve(key).state,Design::ResolutionState::Changed);
  const auto& parameter=captured->semantic->instances.at("/a").parameters.at("width");
  QString detail;
  QVERIFY(!controller->parameterPanel()->applyCaptured(captured,stamp,key,parameter,Value(50.0),false,&detail));
  QCOMPARE(window->activeEditor->toPlainText(),source);
  QTRY_VERIFY(!service->displayCurrent(stamp));
}
void TestSemanticHierarchy::initializeUnstructuredModel(){
  window->inspectorDock->hide();
  auto preview=[&](const QString& source){window->activeEditor->setPlainText(source);QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),15000);QTRY_VERIFY(window->evaluationBinding->current());QTest::qWait(30);};
  const QString source="// Global parameters\nwidth = 12;\nmodule shape() { cube([width, 8, 3]); }\n// Geometry\nshape(); // Keep this comment";
  preview(source);if(QTest::currentTestFailed())return;
  auto *editor=qobject_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(editor);editor->clearUndoHistory();
  auto *panel=window->semanticController->parameterPanel();QVERIFY(panel);
  const auto stamp=window->evaluationBinding->stamp();const auto captured=window->evaluationBinding->scene(stamp);QVERIFY(captured && captured->semantic && !captured->semantic->marked);
  QString detail;editor->qsci->setReadOnly(true);QVERIFY(!panel->initializeCaptured(captured,stamp,"Part",&detail));QVERIFY(detail.contains("read-only"));editor->qsci->setReadOnly(false);QCOMPARE(editor->toPlainText(),source);QVERIFY(!editor->canUndo());
  auto *overlay=window->qglview->findChild<QWidget *>("modelStructureOverlay");QVERIFY(overlay);QVERIFY(!overlay->isVisible());
  QVERIFY(!window->qglview->findChild<QPushButton *>("viewportInitializeStructure"));
  auto *info=window->qglview->findChild<QWidget *>("viewportStatistics");QVERIFY(info);QCOMPARE(info->pos(),QPoint(10,10));
  QVERIFY(window->inspectorDock->isHidden());window->inspectorDock->show();window->inspectorDock->raise();
  QPointer<QPushButton> button=panel->findChild<QPushButton *>("semanticInitializeStructure");QVERIFY(button);QVERIFY(button->isVisible());
  QTimer::singleShot(0,window,[]{if(auto *dialog=qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))dialog->reject();});
  button->click();QCOMPARE(editor->toPlainText(),source);QVERIFY(!editor->canUndo());
  QTimer::singleShot(0,window,[]{if(auto *dialog=qobject_cast<QInputDialog *>(QApplication::activeModalWidget())){dialog->setTextValue(QString::fromUtf8("Housing \"α\""));dialog->accept();}});
  button=panel->findChild<QPushButton *>("semanticInitializeStructure");QVERIFY(button);
  QSignalSpy done(window,&MainWindow::compilationDone);button->click();QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),15000);
  QTRY_VERIFY(window->evaluationBinding->current());const auto result=window->evaluationBinding->scene(window->evaluationBinding->stamp());QVERIFY(result && result->semantic && result->semantic->valid());QCOMPARE(result->semantic->instances.size(),size_t(1));
  QCOMPARE(QString::fromStdString(result->semantic->instances.begin()->second.label),QString::fromUtf8("Housing \"α\""));
  const auto edited=editor->toPlainText();QVERIFY(edited.startsWith("// Global parameters\nwidth = 12;\nmodule shape() { cube([width, 8, 3]); }"));QVERIFY(edited.contains("shape(); // Keep this comment\n}"));
  auto *service=window->semanticController->targetService();auto key=Semantic::TargetKey{Semantic::TargetRole::Instance,result->semantic->instances.begin()->first};
  QTRY_VERIFY(service->resolve(key).state==Design::ResolutionState::Resolved);QCOMPARE(service->resolve(key).worldBounds->max().x(),12.0);
  QTRY_VERIFY(button.isNull());QTRY_VERIFY(overlay->isVisible());editor->undo();QCOMPARE(editor->toPlainText(),source);QVERIFY(!editor->canUndo());editor->redo();QCOMPARE(editor->toPlainText(),edited);
  for(const auto& unsafe:{QString("cube(1); x=3;"),QString("{cube(1);}"),QString("cube(1); module trailing(){}"),QString("echo($parent_modules); cube(1);"),QString("#cube(1);"),QString("// no geometry\nx=1;")}){
    preview(unsafe);if(QTest::currentTestFailed())return;editor->clearUndoHistory();
    const auto currentStamp=window->evaluationBinding->stamp();const auto current=window->evaluationBinding->scene(currentStamp);
    QVERIFY(!panel->initializeCaptured(current,currentStamp,"Part",&detail));QVERIFY(!detail.isEmpty());QCOMPARE(editor->toPlainText(),unsafe);QVERIFY(!editor->canUndo());
  }
  preview(source);if(QTest::currentTestFailed())return;
  QVERIFY(!overlay->isVisible());QCOMPARE(info->pos(),QPoint(10,10));
  const auto oldStamp=window->evaluationBinding->stamp();const auto oldScene=window->evaluationBinding->scene(oldStamp);editor->setPlainText(source+"\n// changed");editor->clearUndoHistory();
  QVERIFY(!panel->initializeCaptured(oldScene,oldStamp,"Part",&detail));QCOMPARE(editor->toPlainText(),source+"\n// changed");QVERIFY(!editor->canUndo());
}
void TestSemanticHierarchy::viewportStructureInteractions(){
  evaluate(pair);if(QTest::currentTestFailed())return;
  auto *controller=window->semanticController;auto *view=window->qglview;auto *tree=controller->treeView();
  auto *overlay=view->findChild<QWidget *>("modelStructureOverlay");QVERIFY(overlay);QVERIFY(overlay->isVisible());
  QCOMPARE(overlay->parentWidget(),view);QVERIFY(view->rect().contains(overlay->geometry()));
  auto *info=view->findChild<QWidget *>("viewportStatistics");QVERIFY(info);
  QVERIFY(info->x()>overlay->geometry().right());QCOMPARE(info->y(),overlay->y());
  QVERIFY(!tree->viewport()->autoFillBackground());
  auto *toggle=window->findChild<QAction *>("viewModelStructure");QVERIFY(toggle);QVERIFY(toggle->isChecked());
  auto *collapse=overlay->findChild<QToolButton *>("modelStructureCollapse");QVERIFY(collapse);
  const auto right=controller->model()->find(Semantic::TreeRole::Instance,"/pair/right");
  tree->scrollTo(right);QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(right).center());
  QVERIFY(controller->selectedTarget());QCOMPARE(controller->selectedTarget()->id,std::string("/pair/right"));
  QTest::keyClick(tree,Qt::Key_Space);QTRY_VERIFY(controller->visibility().hidden.count("/pair/right"));
  QTest::keyClick(tree,Qt::Key_Space);QTRY_VERIFY(!controller->visibility().hidden.count("/pair/right"));
  bool menuAction=false;
  QTimer::singleShot(0,tree,[&]{for(auto *menu:tree->findChildren<QMenu *>())if(menu->isVisible()){
    for(auto *action:menu->actions())if(action->text()=="Solo selected instance" && action->isEnabled()){action->trigger();menuAction=true;break;}menu->close();
  }});
  emit tree->customContextMenuRequested(tree->visualRect(right).center());
  QVERIFY(menuAction);QVERIFY(controller->visibility().solo);QCOMPARE(*controller->visibility().solo,std::string("/pair/right"));QVERIFY(controller->setSolo({}));
  const int expandedHeight=overlay->height();QTest::mouseClick(collapse,Qt::LeftButton);QVERIFY(!tree->isVisible());QVERIFY(overlay->height()<expandedHeight);
  QTest::mouseClick(collapse,Qt::LeftButton);QVERIFY(tree->isVisible());QCOMPARE(controller->selectedTarget()->id,std::string("/pair/right"));
  toggle->trigger();QVERIFY(!overlay->isVisible());QCOMPARE(info->pos(),QPoint(10,10));
  toggle->trigger();QVERIFY(overlay->isVisible());QVERIFY(info->x()>overlay->geometry().right());
  const auto originalSize=window->size();window->resize(800,600);QTest::qWait(30);QVERIFY(view->rect().contains(overlay->geometry()));window->resize(originalSize);
  const auto *originalScheme=view->colorscheme;ColorScheme scheme=*originalScheme;
  const auto restoreScheme=qScopeGuard([&]{view->setColorScheme(*originalScheme);view->update();});
  for(bool dark:{false,true}){
    scheme[RenderColor::BACKGROUND_COLOR]=scheme[RenderColor::BACKGROUND_STOP_COLOR]=dark?Color4f(0.02f,0.02f,0.02f,1.0f):Color4f(0.98f,0.98f,0.98f,1.0f);
    view->setColorScheme(scheme);view->update();QTest::qWait(60);
    QCOMPARE(tree->palette().color(QPalette::Text),QColor(dark?Qt::white:Qt::black));
    const auto composed=view->grab().toImage();const auto model=view->grabFramebuffer();
    const auto point=overlay->mapTo(view,QPoint(overlay->width()-3,2));
    const auto a=composed.pixelColor(point*composed.devicePixelRatio()),b=model.pixelColor(point*model.devicePixelRatio());
    QVERIFY(std::abs(a.red()-b.red())<3 && std::abs(a.green()-b.green())<3 && std::abs(a.blue()-b.blue())<3);
    const auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");if(!output.isEmpty())QVERIFY(composed.save(output+(dark?"/structure-dark.png":"/structure-light.png")));
  }
  view->setColorScheme(*originalScheme);view->update();
}
void TestSemanticHierarchy::parameterWidgetsAndUndo(){
  const auto source=part+"@{type:Component,id:\"a\"} bracket();";
  evaluate(source);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;
  QVERIFY(controller->select({Semantic::TargetRole::Instance,"/a"}));auto *panel=controller->parameterPanel();QVERIFY(panel);
  auto *row=panel->findChild<QWidget *>("semanticParameter-width");QVERIFY(row);auto *spin=row->findChild<QDoubleSpinBox *>();QVERIFY(spin);
  auto *apply=row->findChild<QPushButton *>("semanticApply-width");QVERIFY(apply);QVERIFY(!apply->isEnabled());
  auto *editor=window->activeEditor;editor->clearUndoHistory();QVERIFY(editor->annotations->add(parameterAnnotation()));
  QSignalSpy handoff(editor,&EditorInterface::externalEditStarting);spin->setValue(40);QVERIFY(apply->isEnabled());apply->click();
  QTRY_VERIFY(window->evaluationBinding->current());QTRY_VERIFY(controller->selectedTarget() && controller->selectedTarget()->id=="/a");
  const auto changed=editor->toPlainText();QCOMPARE(changed,part+"@{type:Component,id:\"a\"} bracket(width = 40);");QCOMPARE(handoff.size(),1);
  QCOMPARE(window->evaluationBinding->scene(window->evaluationBinding->stamp())->semantic->instances.at("/a").parameters.at("width").displayedValue,std::string("40"));
  QVERIFY(!editor->parameterWidget->isModified());QCOMPARE(editor->annotations->records().size(),1);
  editor->undo();QCOMPARE(editor->toPlainText(),source);QCOMPARE(editor->annotations->records().size(),1);
  editor->undo();QVERIFY(editor->annotations->records().isEmpty());QCOMPARE(editor->toPlainText(),source);
  editor->redo();QCOMPARE(editor->annotations->records().size(),1);editor->redo();QCOMPARE(editor->toPlainText(),changed);
  evaluate(changed);if(QTest::currentTestFailed())return;QVERIFY(controller->select({Semantic::TargetRole::Instance,"/a"}));
  auto *reset=controller->parameterPanel()->findChild<QPushButton *>("semanticReset-width");QVERIFY(reset);reset->click();QCOMPARE(editor->toPlainText(),source);
  evaluate(part+"@{type:Component,id:\"a\"} bracket(width=30+10);");if(QTest::currentTestFailed())return;QVERIFY(controller->select({Semantic::TargetRole::Instance,"/a"}));
  row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-width");QVERIFY(row);QVERIFY(!row->findChild<QDoubleSpinBox *>());
  auto *mode=controller->parameterPanel()->findChild<QComboBox *>("semanticParameterMode");QVERIFY(mode);mode->setCurrentIndex(2);
  QVERIFY(!controller->parameterPanel()->findChild<QDoubleSpinBox *>());
}
void TestSemanticHierarchy::sharedCallsAndDefaultEdits(){
  const auto rack=pairDefinition+"@{type:Assembly,id:\"rack-def\"} module rack(width=70){@{type:Component,id:\"pair\"} pair(width=width,gap=60);}\n@{type:Component,id:\"rack-a\"} rack(width=70); translate([180,0,0]) @{type:Component,id:\"rack-b\"} rack();";
  evaluate(rack);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;
  QVERIFY(controller->select({Semantic::TargetRole::Instance,"/rack-a"}));auto *row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-width");QVERIFY(row->findChild<QDoubleSpinBox *>());
  QVERIFY(controller->setHidden("/rack-b",true));QVERIFY(controller->select({Semantic::TargetRole::Instance,"/rack-a/pair"}));
  row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-gap");QVERIFY(row);QVERIFY(!row->findChild<QDoubleSpinBox *>());
  QVERIFY(controller->select({Semantic::TargetRole::Instance,"/rack-a/pair/right"}));row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-width");QVERIFY(row);QVERIFY(!row->findChild<QDoubleSpinBox *>());
  const auto source=part+"@{type:Component,id:\"a\"} bracket(); translate([70,0,0]) @{type:Component,id:\"b\"} bracket(width=50);";
  evaluate(source);if(QTest::currentTestFailed())return;QVERIFY(controller->select({Semantic::TargetRole::Definition,"bracket"}));
  QTRY_VERIFY(controller->parameterPanel()->findChild<QDoubleSpinBox *>());
  row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-width");auto *spin=row->findChild<QDoubleSpinBox *>();QVERIFY(spin);
  QVERIFY(controller->parameterPanel()->findChild<QLabel *>("semanticParameterStatus")->text().contains("every use"));
  spin->setValue(44);auto *apply=row->findChild<QPushButton *>("semanticApply-width");QVERIFY(apply);apply->click();
  QTRY_VERIFY(window->evaluationBinding->current());const auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());
  QCOMPARE(scene->semantic->instances.at("/a").parameters.at("width").displayedValue,std::string("44"));QCOMPARE(scene->semantic->instances.at("/b").parameters.at("width").displayedValue,std::string("50"));
  auto expected=source;expected.replace("module bracket(width=30)","module bracket(width=44)");QCOMPARE(window->activeEditor->toPlainText(),expected);
}
void TestSemanticHierarchy::parameterOwnerAndContextGuards(){
  QTemporaryDir directory;QVERIFY(directory.isValid());const auto library=directory.filePath("library.scad"),rootPath=directory.filePath("root.scad");
  const auto libraryBytes=part.toUtf8();const QString root="use <library.scad>\n@{type:Component,id:\"a\"} bracket();";
  for(const auto& entry:std::vector<std::pair<QString,QByteArray>>{{library,libraryBytes},{rootPath,root.toUtf8()}}){QFile file(entry.first);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(entry.second),entry.second.size());}
  window->tabManager->open(rootPath);auto *rootEditor=window->activeEditor;evaluate(root);if(QTest::currentTestFailed())return;
  auto *controller=window->semanticController;auto key=Semantic::TargetKey{Semantic::TargetRole::Definition,"bracket"};QVERIFY(controller->select(key));
  QTRY_VERIFY(controller->targetService()->resolve(key).state==Design::ResolutionState::Resolved);
  QVERIFY(!controller->parameterPanel()->findChild<QDoubleSpinBox *>());
  window->tabManager->open(library);auto *libraryEditor=qobject_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(libraryEditor);
  auto *tabs=qobject_cast<QTabWidget *>(window->tabManager->getTabContent());QVERIFY(tabs);tabs->setCurrentWidget(rootEditor);evaluate(root);if(QTest::currentTestFailed())return;QVERIFY(controller->select(key));
  QTRY_VERIFY(controller->parameterPanel()->findChild<QDoubleSpinBox *>());
  auto *panel=controller->parameterPanel();auto captured=window->evaluationBinding->scene(window->evaluationBinding->stamp());auto stamp=window->evaluationBinding->stamp();
  auto parameter=controller->targetService()->resolve(key).parameters.at("width");QString detail;libraryEditor->clearUndoHistory();rootEditor->clearUndoHistory();
  libraryEditor->qsci->setReadOnly(true);QVERIFY(!panel->applyCaptured(captured,stamp,key,parameter,Value(40.0),false,&detail));QVERIFY(detail.contains("read-only"));libraryEditor->qsci->setReadOnly(false);QVERIFY(!libraryEditor->canUndo());
  const auto connection=connect(libraryEditor,&EditorInterface::externalEditStarting,window,[&]{tabs->setCurrentWidget(libraryEditor);});
  QVERIFY(!panel->applyCaptured(captured,stamp,key,parameter,Value(40.0),false,&detail));disconnect(connection);
  QCOMPARE(libraryEditor->toPlainText().toUtf8(),libraryBytes);QVERIFY(!libraryEditor->canUndo());QCOMPARE(rootEditor->toPlainText(),root);
  tabs->setCurrentWidget(rootEditor);evaluate(root);if(QTest::currentTestFailed())return;QVERIFY(controller->select(key));QTRY_VERIFY(controller->targetService()->resolve(key).state==Design::ResolutionState::Resolved);
  panel=controller->parameterPanel();captured=window->evaluationBinding->scene(window->evaluationBinding->stamp());stamp=window->evaluationBinding->stamp();parameter=controller->targetService()->resolve(key).parameters.at("width");
  QVERIFY2(panel->applyCaptured(captured,stamp,key,parameter,Value(40.0),false,&detail),qPrintable(detail));QTRY_VERIFY(window->evaluationBinding->current());QCOMPARE(rootEditor->toPlainText(),root);QVERIFY(libraryEditor->toPlainText().contains("width=40"));
  auto live=libraryEditor->toPlainText();QVERIFY(!panel->applyCaptured(captured,stamp,key,parameter,Value(50.0)));QCOMPARE(libraryEditor->toPlainText(),live);
  tabs->setCurrentWidget(libraryEditor);libraryEditor->setContentModified(false);window->tabManager->closeCurrentTab();
  QVERIFY(!panel->applyCaptured(captured,stamp,key,parameter,Value(50.0)));QCOMPARE(rootEditor->toPlainText(),root);
}
void TestSemanticHierarchy::closeWindowWithPendingTargets(){
  auto extra=std::make_unique<MainWindow>(QStringList{});extra->designActionAutoReload->setChecked(false);extra->show();
  QVERIFY(QTest::qWaitForWindowExposed(extra.get()));QTRY_VERIFY(extra->qglview->isValid());QTest::qWait(30);
  extra->activeEditor->setPlainText(pair);
  QSignalSpy evaluated(extra.get(),&MainWindow::compilationDone);extra->actionRenderPreview();QTRY_VERIFY(!evaluated.empty());QTRY_COMPARE(extra->inspectionPresentation->status(),InspectionPresentationController::Status::Ready);
  auto captured=extra->evaluationBinding->scene(extra->evaluationBinding->stamp());QVERIFY(captured);
  std::promise<void> held,release;auto gate=release.get_future().share();auto blocker=std::async(std::launch::async,[&]{NativeExecutionGuard native;held.set_value();gate.wait();});
  struct Release {std::promise<void>& promise;bool released=false;~Release(){if(!released)promise.set_value();}} cleanup{release};held.get_future().wait();
  QPointer<Semantic::TargetService> service=extra->semanticController->targetService();service->resolve({Semantic::TargetRole::Definition,"pair-def"});QTest::qWait(20);
  extra.reset();QVERIFY(!service);release.set_value();cleanup.released=true;blocker.get();QTest::qWait(100);
  const auto target=Semantic::prepareTarget(captured->semantic,{Semantic::TargetRole::Instance,"/pair/right"},"after-window-close",captured->documentPath);
  QVERIFY(target.state==Design::ResolutionState::Resolved);QCOMPARE(target.localBounds->max().x(),45.0);
}
void TestSemanticHierarchy::typedParameterControlsAndGlobalCustomizer(){
  auto *panel=window->semanticController->parameterPanel();QVERIFY(panel);
  QVERIFY(window->inspectorDock->isAncestorOf(panel));
  QCOMPARE(window->parameterDock->widget(),static_cast<QWidget *>(window->activeEditor->parameterWidget));
  QVERIFY(!window->parameterDock->isAncestorOf(panel));
  QVERIFY(!window->findChild<QTabWidget *>("semanticCustomizerTabs"));
  window->inspectorDock->refresh();QCOMPARE(window->semanticController->parameterPanel(),panel);
  QString source="@{type:Part,id:\"typed\"} module typed(flag=true,label=\"α\",dims=[12,3,2,1,0.12345678901234567]){cube([dims[0],dims[1],dims[2]]);}\n@{type:Component,id:\"a\"} typed();";
  evaluate(source);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;QVERIFY(controller->select({Semantic::TargetRole::Instance,"/a"}));
  auto *row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-flag");auto *check=row->findChild<QCheckBox *>();QVERIFY(check);check->click();row->findChild<QPushButton *>("semanticApply-flag")->click();
  QTRY_VERIFY(window->evaluationBinding->current());QCOMPARE(window->evaluationBinding->scene(window->evaluationBinding->stamp())->semantic->instances.at("/a").parameters.at("flag").displayedValue,std::string("false"));
  QTRY_VERIFY(controller->selectedTarget() && controller->parameterPanel()->findChild<QWidget *>("semanticParameter-label"));
  row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-label");QVERIFY(row);auto *line=row->findChild<QLineEdit *>();QVERIFY(line);line->setFocus();line->selectAll();
  QInputMethodEvent input;input.setCommitString(QString::fromUtf8("β \"quoted\""));QApplication::sendEvent(line,&input);
  row->findChild<QPushButton *>("semanticApply-label")->click();QTRY_VERIFY(window->evaluationBinding->current());
  QCOMPARE(window->evaluationBinding->scene(window->evaluationBinding->stamp())->semantic->instances.at("/a").parameters.at("label").displayedValue,std::string("β \"quoted\""));
  QTRY_VERIFY(controller->selectedTarget() && controller->parameterPanel()->findChild<QWidget *>("semanticParameter-dims"));
  row=controller->parameterPanel()->findChild<QWidget *>("semanticParameter-dims");QVERIFY(row);const auto values=row->findChildren<ParameterVirtualWidget *>();QCOMPARE(values.size(),2);
  auto *first=static_cast<VectorParameter *>(values.front()->getParameter());auto *second=static_cast<VectorParameter *>(values.back()->getParameter());QCOMPARE(second->value.front(),0.12345678901234567);
  auto *spin=values.front()->findChild<QDoubleSpinBox *>("doubleSpinBox1");QVERIFY(spin);spin->setValue(16);QCOMPARE(first->value.front(),16.0);
  row->findChild<QPushButton *>("semanticApply-dims")->click();QTRY_VERIFY(window->evaluationBinding->current());const auto geometry=controller->targetService()->resolve({Semantic::TargetRole::Instance,"/a"});
  const auto parameters=window->evaluationBinding->scene(window->evaluationBinding->stamp())->semantic->instances.at("/a").parameters;
  QCOMPARE(parameters.at("dims").value->toVector()[4].toDouble(),0.12345678901234567);QVERIFY(window->activeEditor->toPlainText().contains("dims = [16, 3, 2, 1, 0.12345678901234566]"));QVERIFY(!window->activeEditor->parameterWidget->isModified());
  source="global_width=30; // [10:1:100]\n@{type:Part,id:\"global-part\"} module global_part(width=global_width){cube([width,10,3]);}\n@{type:Component,id:\"global\"} global_part();";
  evaluate(source);if(QTest::currentTestFailed())return;QVERIFY(controller->select({Semantic::TargetRole::Instance,"/global"}));
  auto *global=window->activeEditor->parameterWidget;ParameterVirtualWidget *widget=nullptr;for(auto *candidate:global->findChildren<ParameterVirtualWidget *>())if(candidate->getParameter()->name()=="global_width"){widget=candidate;break;}QVERIFY(widget);
  auto captured=window->evaluationBinding->scene(window->evaluationBinding->stamp());auto stamp=window->evaluationBinding->stamp();const auto oldParameter=captured->semantic->instances.at("/global").parameters.at("width");
  auto *number=dynamic_cast<NumberParameter *>(widget->getParameter());QVERIFY(number);number->value=40;widget->setValue();global->checkBoxAutoPreview->setChecked(true);
  QSignalSpy evaluated(window,&MainWindow::compilationDone);QVERIFY(QMetaObject::invokeMethod(widget,"changed",Qt::DirectConnection,Q_ARG(bool,true)));QTRY_VERIFY(!evaluated.empty());QTRY_VERIFY(window->evaluationBinding->current());
  QCOMPARE(window->activeEditor->toPlainText(),source);QCOMPARE(window->evaluationBinding->scene(window->evaluationBinding->stamp())->semantic->instances.at("/global").parameters.at("width").displayedValue,std::string("40"));
  QVERIFY(!controller->parameterPanel()->applyCaptured(captured,stamp,{Semantic::TargetRole::Instance,"/global"},oldParameter,Value(50.0)));QCOMPARE(window->activeEditor->toPlainText(),source);
  global->checkBoxAutoPreview->setChecked(false);global->setModified(false);
}
void TestSemanticHierarchy::pickingAndMasks(){
  evaluate(pair);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;
  for(auto candidate:std::vector<std::pair<Vector3d,std::string>>{{{20,0,1.5},"/pair/left"},{{80,0,1.5},"/pair/right"},{{15,6,1.5},"/pair/left"}}){
    auto hit=pick(window,candidate.first,{0,-1,0});QVERIFY2(hit.hit,qPrintable(hit.detail));QCOMPARE(controller->pathForDraw(hit.hit->object.drawId).value(),candidate.second);QCOMPARE(controller->selectedTarget()->id,candidate.second);
  }
  auto key=Semantic::TargetKey{Semantic::TargetRole::Instance,"/pair/right"};QTRY_VERIFY(controller->targetService()->resolve(key).state==Design::ResolutionState::Resolved);
  const auto source=window->activeEditor->toPlainText();auto meshes=window->inspectionPresentation->meshes();auto renderer=window->qglview->renderer;auto jobs=controller->meshJobs();QSignalSpy evaluated(window,&MainWindow::compilationDone);
  for(int i=0;i<100;++i)QVERIFY(controller->setHidden("/pair/right",i%2==0));
  QCOMPARE(controller->meshJobs(),jobs);QCOMPARE(window->inspectionPresentation->meshes(),meshes);QCOMPARE(window->qglview->renderer,renderer);QCOMPARE(evaluated.size(),0);QCOMPARE(window->activeEditor->toPlainText(),source);
  QVERIFY(controller->setHidden("/pair/right",true));auto hidden=pick(window,{80,0,1.5},{0,-1,0});QVERIFY(!hidden.hit);
  QVERIFY(controller->setSolo("/pair/left"));QVERIFY(!pick(window,{80,0,1.5},{0,-1,0}).hit);QVERIFY(pick(window,{20,0,1.5},{0,-1,0}).hit);QVERIFY(controller->setSolo({}));QVERIFY(!pick(window,{80,0,1.5},{0,-1,0}).hit);
  QTemporaryDir output;QSignalSpy exported(controller->targetService(),&Semantic::TargetService::exportFinished);
  auto info=createExportInfo(FileFormat::ASCII_STL,fileformat::info(FileFormat::ASCII_STL),"target.scad",nullptr,{});
  QVERIFY(controller->targetService()->exportTarget(key,Semantic::ExportSpace::AsPlaced,output.path()+"/hidden-target.stl",info));QTRY_VERIFY(!exported.empty());QVERIFY(exported.back().at(0).toBool());
  QFile file(output.path()+"/hidden-target.stl");QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(file.size()>100);
  QVERIFY(controller->setHidden("/pair/right",false));QVERIFY(pick(window,{80,0,1.5},{0,-1,0}).hit);
}
void TestSemanticHierarchy::transformedFinishedParts(){
  for(auto backend:{RenderBackend3D::ManifoldBackend,RenderBackend3D::CGALBackend}){RenderSettings::inst()->backend3D=backend;
    evaluate(part+"mirror([1,0,0]) translate([60,0,0]) scale([2,1,1]) @{type:Component,id:\"mirror\"} bracket(); translate([0,20,0]) @{type:Component,id:\"back\"} bracket(); @{type:Component,id:\"front\"} bracket(); translate([200,0,0]) cube(2);",true);if(QTest::currentTestFailed())return;
    auto mirrored=pick(window,{-100,0,1.5},{0,-1,0});QVERIFY2(mirrored.hit,qPrintable(mirrored.detail));QCOMPARE(window->semanticController->pathForDraw(mirrored.hit->object.drawId).value(),std::string("/mirror"));
    auto occluded=pick(window,{20,0,1.5},{0,-1,0});QVERIFY(occluded.hit);QCOMPARE(window->semanticController->pathForDraw(occluded.hit->object.drawId).value(),std::string("/front"));
    auto other=pick(window,{201,0,1},{0,-1,0});QVERIFY(other.hit);QVERIFY(!window->semanticController->pathForDraw(other.hit->object.drawId));QVERIFY(!window->semanticController->selectedTarget());
  }
}
void TestSemanticHierarchy::effectsAndToolArbitration(){
  evaluate(pair);if(QTest::currentTestFailed())return;QObject lifetime;auto probe=std::make_shared<EffectProbe>();
  auto effect=window->inspectionPresentation->registerMeshEffect("semantic identity proof",10,&lifetime,[probe](auto child){return std::make_shared<IdentityEffect>(std::move(child),probe);});QVERIFY(effect);
  auto hit=pick(window,{80,0,1.5},{0,-1,0});QVERIFY(hit.hit);QCOMPARE(window->semanticController->pathForDraw(hit.hit->object.drawId).value(),std::string("/pair/right"));QVERIFY(probe->picks>0);QVERIFY(probe->colors>0);
  auto *tools=window->viewportTools;int deliveries=0;auto token=tools->beginPick({"semantic arbitration",window->evaluationBinding->stamp(),true},&lifetime,[&](auto){++deliveries;});QVERIFY(token);
  auto *view=window->qglview;auto before=view->navigation()->current();QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,view->rect().center());
  QMouseEvent motion(QEvent::MouseMove,QPointF(view->rect().center()+QPoint(35,25)),QPointF(view->mapToGlobal(view->rect().center()+QPoint(35,25))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(view,&motion);QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,view->rect().center()+QPoint(35,25));
  QCOMPARE(deliveries,0);QVERIFY(!(before==view->navigation()->current()));QCOMPARE(tools->active(),token);QTest::keyClick(view,Qt::Key_Escape);QCOMPARE(deliveries,1);
}
void TestSemanticHierarchy::retainedJobsAndStaleDelivery(){
  evaluate(pair);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;auto *service=controller->targetService();
  auto key=Semantic::TargetKey{Semantic::TargetRole::Definition,"pair-def"};auto captured=window->evaluationBinding->scene(window->evaluationBinding->stamp());
  std::promise<void> held,release;auto gate=release.get_future().share();auto blocker=std::async(std::launch::async,[&]{NativeExecutionGuard execution;held.set_value();gate.wait();});held.get_future().wait();
  service->resolve(key);QTest::qWait(20);window->activeEditor->setPlainText(pair+"\n// changed while preparing");QTest::qWait(10);release.set_value();blocker.get();
  QTRY_VERIFY(service->resolve(key).state==Design::ResolutionState::Changed);QVERIFY(!controller->selectedTarget());
  auto retained=std::async(std::launch::async,[captured]{return Semantic::prepareTarget(captured->semantic,{Semantic::TargetRole::Instance,"/pair/right"},"retained",captured->documentPath);});
  auto target=retained.get();QVERIFY(target.state==Design::ResolutionState::Resolved);QCOMPARE(target.localBounds->max().x(),45.0);
  evaluate(pair);if(QTest::currentTestFailed())return;QVERIFY(controller->select({Semantic::TargetRole::Instance,"/pair/right"}));QVERIFY(controller->setHidden("/pair/right",true));
  window->tabManager->actionNew();window->activeEditor->setPlainText("cube(1);");window->actionRenderPreview();QTRY_VERIFY(window->evaluationBinding->current());QTest::qWait(20);QVERIFY(!controller->selectedTarget());
  window->activeEditor->setContentModified(false);window->tabManager->closeCurrentTab();QTest::qWait(20);
  evaluate(pair);if(QTest::currentTestFailed())return;QVERIFY(controller->visibility().hidden.count("/pair/right"));
}
void TestSemanticHierarchy::nativePlacementAndDefaults(){
  std::atomic<int> calls{0};auto registration=Semantic::registerPlacementPass("native hierarchy proof",900,[&](auto&){++calls;return Semantic::PlacementOutcome{};});QVERIFY(registration);
  evaluate(pair);if(QTest::currentTestFailed())return;QCOMPARE(calls.load(),1);
  auto *service=window->semanticController->targetService();auto key=Semantic::TargetKey{Semantic::TargetRole::Definition,"bracket"};service->resolve(key);QTRY_VERIFY(service->resolve(key).state==Design::ResolutionState::Resolved);QCOMPARE(calls.load(),2);
  for(int i=0;i<10;++i)service->resolve(key);QTest::qWait(10);QCOMPARE(calls.load(),2);
  auto jobs=window->semanticController->meshJobs();window->qglview->navigation()->orient({40,0,30});QTest::qWait(20);QCOMPARE(window->semanticController->meshJobs(),jobs);QCOMPARE(calls.load(),2);
}
void TestSemanticHierarchy::sourceNavigationAndSection(){
  evaluate(pair);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;QVERIFY(controller->select({Semantic::TargetRole::Instance,"/pair/right"}));
  auto key=Semantic::TargetKey{Semantic::TargetRole::Instance,"/pair/right"};QTRY_VERIFY(controller->targetService()->resolve(key).state==Design::ResolutionState::Resolved);
  auto *section=window->inspectorDock->findChild<QPushButton *>("semanticCreateSection");QVERIFY(section);QVERIFY(section->isEnabled());section->click();
  auto *document=Design::DesignDocument::forEditor(*window->activeEditor,*window);QCOMPARE(document->transientRecords().size(),size_t(1));QVERIFY(window->activeEditor->toPlainText().contains("@{type:Component"));QVERIFY(!window->activeEditor->toPlainText().contains("type:Section"));
  const auto sectionRecord=document->transientRecords().begin()->second;
  QVERIFY(sectionRecord->dependencies.empty());
  QVERIFY(!sectionRecord->definition.toObject().contains("target"));
  QVERIFY(!sectionRecord->definition.toObject().contains("frame"));
  const auto target=controller->targetService()->resolve(key);
  Design::ResolveContext context{"", "", "", BoundingBox(), document->transientRecords(), {}};
  const auto pose=Design::sectionFrame(*sectionRecord,context);
  QVERIFY(pose.frame);QVERIFY(target.localBounds);
  QVERIFY(pose.frame->translation().isApprox(target.localToWorld*target.localBounds->center()));
  QVERIFY(pose.frame->linear().col(2).isApprox(target.localToWorld.linear()*Vector3d::UnitZ()));
  auto *call=window->inspectorDock->findChild<QPushButton *>("semanticGoToCall");QVERIFY(call);call->click();auto *editor=qobject_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(editor);QVERIFY(editor->qsci->selectedText().contains("bracket"));
  QVERIFY(controller->select({Semantic::TargetRole::Definition,"bracket"}));auto *definition=window->inspectorDock->findChild<QPushButton *>("semanticGoToDefinition");QVERIFY(definition);definition->click();QVERIFY(editor->qsci->selectedText().contains("bracket"));
}
