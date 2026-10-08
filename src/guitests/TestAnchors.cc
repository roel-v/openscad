#include "TestAnchors.h"
#include "core/anchors/AnchorOperations.h"
#include "gui/anchors/AnchorController.h"
#include "gui/ScintillaEditor.h"
#include "gui/semantic/SemanticController.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/InspectionPresentationController.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "glview/RenderSettings.h"
#include <QFile>
#include <QSignalSpy>
#include <QTest>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QStatusBar>
#include <QLabel>
#include <QBuffer>
#include <QDateTime>
#include <QJsonArray>
#include <QUuid>
#include <QScrollArea>
namespace {
struct ProjectionSnapshot {
  Eigen::Matrix4d worldToClip;
  int width,height;
  qreal pixelRatio;
  QPointF operator()(const Vector3d& point) const {
    Vector4d clip=worldToClip*Vector4d(point.x(),point.y(),point.z(),1);clip/=clip.w();
    return {((clip.x()+1)*width/2)/pixelRatio,((1-clip.y())*height/2)/pixelRatio};
  }
};
ProjectionSnapshot projectionSnapshot(QGLView& view) {
  view.makeCurrent();view.setupCamera();const Eigen::Map<const Eigen::Matrix4d> model(view.modelview),projection(view.projection);
  ProjectionSnapshot snapshot{projection*model,view.cam.pixel_width,view.cam.pixel_height,view.devicePixelRatio()};view.doneCurrent();return snapshot;
}
QPointF projected(QGLView& view,const Vector3d& point) { return projectionSnapshot(view)(point); }
AgentAnnotations::Record undoAnnotation(){
  QImage image(8,8,QImage::Format_RGB32);image.fill(Qt::blue);QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");
  return {{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"comment","anchor joint undo"},{"created_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    {"width",8},{"height",8},{"png_sha256",QString::fromLatin1(AgentAnnotations::Store::hash(png))},
    {"camera",QJsonObject{{"translation",QJsonArray{0,0,0}},{"rotation",QJsonArray{0,0,0}},{"distance",100},{"fov",45},{"projection","perspective"}}},{"strokes",QJsonArray{}},{"result",QJsonObject{}}},png};
}
}
void TestAnchors::initTestCase() {
  window->hide();window->show();window->raise();window->activateWindow();QVERIFY(QTest::qWaitForWindowExposed(window));QTRY_VERIFY(window->qglview->isValid());
  const auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");if(!output.isEmpty()){window->qglview->makeCurrent();auto info=window->qglview->getRendererInfo();window->qglview->doneCurrent();QFile file(output+"/renderer-info-anchors.txt");QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write(QByteArray::fromStdString(info))>0);}
}
void TestAnchors::init() {
  for(auto *editor:window->tabManager->editorList)editor->setContentModified(false);restoreWindowInitialState();window->designActionAutoReload->setChecked(false);
  // Visibility and unsaved drafts belong to a document; every case starts with a new document.
  window->tabManager->actionNew();
  previousBackend=static_cast<int>(RenderSettings::inst()->backend3D);RenderSettings::inst()->backend3D=RenderBackend3D::ManifoldBackend;window->qglview->navigation()->settings.animate=false;window->show();window->raise();window->activateWindow();
}
void TestAnchors::cleanup() {
  RenderSettings::inst()->backend3D=static_cast<RenderBackend3D>(previousBackend);for(auto *editor:window->tabManager->editorList)editor->setContentModified(false);restoreWindowInitialState();window->designActionAutoReload->setChecked(false);
}
QString TestAnchors::fixture() const {QFile file(fixturePath("scad/anchors/reusable-fixture.scad"));if(!file.open(QIODevice::ReadOnly))return {};return QString::fromUtf8(file.readAll());}
void TestAnchors::evaluate(const QString& source,bool final) {
  QVERIFY(!source.isEmpty());if(window->activeEditor->toPlainText()!=source)window->activeEditor->setPlainText(source);QSignalSpy done(window,&MainWindow::compilationDone);
  if(final)window->designActionRender->trigger();else window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),15000);QTRY_VERIFY_WITH_TIMEOUT(window->evaluationBinding->current(),5000);
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());QVERIFY(scene && scene->semantic);
  if(scene->semantic->marked)QTRY_COMPARE_WITH_TIMEOUT(window->inspectionPresentation->status(),InspectionPresentationController::Status::Ready,15000);
  else QTRY_COMPARE_WITH_TIMEOUT(window->inspectionPresentation->status(),InspectionPresentationController::Status::Inactive,15000);
}
void TestAnchors::nativePreviewRenderAndSolvedPicks() {
  const auto source=fixture();evaluate(source);if(QTest::currentTestFailed())return;
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());QVERIFY(scene);QVERIFY(scene->semantic->instances.at("/fixture-a/peg").localToWorld.translation().isApprox(Vector3d(20,0,10)));
  auto *view=window->qglview;const Vector3d tip(20,0,22);view->navigation()->apply({CameraPose::upright(Vector3d::UnitZ()),-tip,100,45,Camera::ProjectionType::ORTHOGONAL});view->update();QTest::qWait(20);
  // Inspector can resize the viewport after selection; use the captured pick frame.
  const auto pickProjection=projectionSnapshot(*view);const auto pixel=pickProjection(tip);
  auto picked=window->selectionService->pick(pixel,window->evaluationBinding->stamp());QVERIFY2(picked.hit,qPrintable(picked.detail));
  // The GPU reads a pixel center; X/Y must agree within that physical pixel, not an arbitrary subpixel distance.
  QVERIFY(std::abs(picked.hit->worldPoint.z()-tip.z())<1e-4);
  QVERIFY(QLineF(pickProjection(picked.hit->worldPoint),pixel).length()<=1.5/pickProjection.pixelRatio);
  QVERIFY((picked.hit->worldPoint-tip).norm()<1);QVERIFY(picked.hit->outwardNormal.dot(Vector3d::UnitZ())>0.99);
  auto path=window->semanticController->pathForDraw(picked.hit->object.drawId);QVERIFY(path);QCOMPARE(*path,std::string("/fixture-a/peg"));
  evaluate(source,true);if(QTest::currentTestFailed())return;scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());QVERIFY(scene->semantic->instances.at("/fixture-a/peg").localToWorld.translation().isApprox(Vector3d(20,0,10)));
}
void TestAnchors::hiddenTargetsAndParameterReevaluation() {
  const auto source=fixture();evaluate(source);if(QTest::currentTestFailed())return;auto *controller=window->semanticController;auto *service=controller->targetService();
  Semantic::TargetKey target{Semantic::TargetRole::Instance,"/fixture-a/peg"};QTRY_VERIFY(service->resolve(target).state==Design::ResolutionState::Resolved);
  auto prepared=service->resolve(target);QVERIFY(prepared.localBounds->sizes().isApprox(Vector3d(4,4,12)));QVERIFY(prepared.worldBounds->max().isApprox(Vector3d(22,2,22)));
  QVERIFY(controller->setHidden(target.id,true));QVERIFY(controller->setSolo("/fixture-b/plate"));QVERIFY(service->resolve(target).worldBounds->max().isApprox(prepared.worldBounds->max()));
  auto sourceChanged=source;sourceChanged.replace("fixture-a\"} fixture(width = 40)","fixture-a\"} fixture(width = 50)");QVERIFY(sourceChanged!=source);evaluate(sourceChanged);if(QTest::currentTestFailed())return;
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());QVERIFY(scene->semantic->instances.at("/fixture-a/peg").localToWorld.translation().isApprox(Vector3d(25,0,10)));QVERIFY(scene->semantic->instances.at("/fixture-b/peg").localToWorld.translation().isApprox(Vector3d(130,0,10)));
}
void TestAnchors::failedRelationRetainsOnlyAStaleLastGoodResult() {
  auto source=fixture();evaluate(source);if(QTest::currentTestFailed())return;auto retained=window->evaluationBinding->scene(window->evaluationBinding->stamp());
  source+="\n@{type:Attachment} duplicate={id:\"duplicate\",child:{component:\"/fixture-a/peg\",anchor:\"base\"},target:{component:\"/fixture-b/plate\",anchor:\"mount\"}};\n";
  window->activeEditor->setPlainText(source);QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),15000);
  QVERIFY(!window->evaluationBinding->current());QVERIFY(retained->semantic->valid());QVERIFY(retained->semantic->instances.at("/fixture-a/peg").localToWorld.translation().isApprox(Vector3d(20,0,10)));QCOMPARE(window->activeEditor->toPlainText(),source);
}
void TestAnchors::fixedPartCaptureRoutingSaveUndoAndReopen(){
  const auto source=fixture();evaluate(source);if(QTest::currentTestFailed())return;auto controller=window->anchorController;auto editor=window->activeEditor;editor->clearUndoHistory();
  window->inspectorDock->show();window->inspectorDock->raise();const Vector3d tip(20,0,22);auto view=window->qglview;
  view->navigation()->apply({CameraPose::upright(Vector3d::UnitZ()),-tip,100,45,Camera::ProjectionType::ORTHOGONAL});view->update();QTest::qWait(20);
  auto pick=window->inspectorDock->findChild<QPushButton *>("pickAnchor");QVERIFY(pick && pick->isEnabled());QTest::mouseClick(pick,Qt::LeftButton);QVERIFY(window->viewportTools->active());
  QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projected(*view,tip).toPoint());QTRY_VERIFY2(controller->captureDraft(),qPrintable(window->statusBar()->currentMessage()));auto draft=*controller->captureDraft();auto key=Anchors::referenceKey(draft.key);QVERIFY(key);QCOMPARE(key->owner.id,std::string("/fixture-a/peg"));
  auto captured=std::get<Anchors::Anchor>(std::dynamic_pointer_cast<const Anchors::ReferenceDeclaration>(draft.preview->evaluated)->value);QVERIFY(captured.fixedCapture);QVERIFY(std::abs(captured.position.z()-12)<1e-4);QCOMPARE(editor->toPlainText(),source);
  auto cancel=window->inspectorDock->findChild<QPushButton *>("cancelAnchorCapture");QVERIFY(cancel);QTest::mouseClick(cancel,Qt::LeftButton);QVERIFY(!controller->captureDraft());QCOMPARE(editor->toPlainText(),source);
  QVERIFY(controller->beginPick());QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projected(*view,tip).toPoint());QTRY_VERIFY(controller->captureDraft());draft=*controller->captureDraft();key=Anchors::referenceKey(draft.key);captured=std::get<Anchors::Anchor>(std::dynamic_pointer_cast<const Anchors::ReferenceDeclaration>(draft.preview->evaluated)->value);
  view->navigation()->apply({CameraPose::upright(Vector3d::UnitX()),-tip,100,45,Camera::ProjectionType::ORTHOGONAL});auto save=window->inspectorDock->findChild<QPushButton *>("saveAnchor");QVERIFY(save && save->isEnabled());QTest::mouseClick(save,Qt::LeftButton);auto saved=editor->toPlainText();QVERIFY(saved!=source);QVERIFY(saved.contains("/fixture-a/peg"));
  editor->undo();QCOMPARE(editor->toPlainText(),source);QVERIFY(controller->captureDraft());QCOMPARE(controller->captureDraft()->preview->id,draft.preview->id);editor->redo();QCOMPARE(editor->toPlainText(),saved);QVERIFY(!controller->captureDraft());
  evaluate(saved);if(QTest::currentTestFailed())return;auto resolved=controller->resolve(*key);QVERIFY(resolved.frame);auto local=std::get<Anchors::Anchor>(*resolved.local);QVERIFY(local.up.isApprox(captured.up,1e-12));QVERIFY(local.normal.isApprox(captured.normal,1e-12));
  QVERIFY(window->semanticController->setHidden(key->owner.id,true));auto jobs=window->semanticController->meshJobs();QVERIFY(controller->selectReference(*key));QVERIFY(window->semanticController->visibility().hidden.count(key->owner.id));QCOMPARE(window->semanticController->meshJobs(),jobs);
  QTemporaryDir directory;const auto path=directory.filePath("fixed-capture.scad");QVERIFY(window->tabManager->saveAs(editor,path));window->tabManager->closeCurrentTab();window->tabManager->open(path);evaluate(saved);if(QTest::currentTestFailed())return;resolved=controller->resolve(*key);QVERIFY(resolved.frame);QVERIFY(std::get<Anchors::Anchor>(*resolved.local).up.isApprox(captured.up,1e-12));
}
void TestAnchors::anonymousCaptureAndStaleSingularGuards(){
  evaluate("cube(10);\n");if(QTest::currentTestFailed())return;auto controller=window->anchorController;auto view=window->qglview;const Vector3d point(5,5,10);
  view->navigation()->apply({CameraPose::upright(Vector3d::UnitZ()),-point,100,45,Camera::ProjectionType::ORTHOGONAL});view->update();QTest::qWait(20);
  QVERIFY(view->renderer);auto hit=window->selectionService->pick(projected(*view,point),window->evaluationBinding->stamp());QVERIFY2(hit.hit,qPrintable(hit.detail));std::string error;QVERIFY(controller->capture(*hit.hit,{},&error));auto draft=controller->captureDraft();QVERIFY(draft);auto key=Anchors::referenceKey(draft->key);QCOMPARE(key->owner.role,Anchors::OwnerRole::Document);QVERIFY(!draft->preview->definition.toObject().contains("owner"));QVERIFY(controller->saveCapture());auto source=window->activeEditor->toPlainText();evaluate(source);if(QTest::currentTestFailed())return;QVERIFY(controller->resolve(*key).frame);
  auto stale=*hit.hit;QVERIFY(!controller->capture(stale,{},&error));QVERIFY(!controller->captureDraft());
  evaluate("@{type:Part,id:\"piece\"}module piece(){cube(1);}scale([1,1,0])@{type:Component,id:\"singular\"}piece();cube(10);\n");if(QTest::currentTestFailed())return;
  hit=window->selectionService->pick(projected(*view,point),window->evaluationBinding->stamp());QVERIFY(hit.hit);QVERIFY(!controller->capture(*hit.hit,std::string("/singular"),&error));QVERIFY(error.find("singular")!=std::string::npos);QVERIFY(!controller->captureDraft());
}
void TestAnchors::explicitSharedEditsAndAttachmentControls(){
  auto source=fixture();evaluate(source);if(QTest::currentTestFailed())return;auto controller=window->anchorController;window->inspectorDock->show();
  Anchors::ReferenceKey mount{Anchors::instanceOwner("/fixture-a/plate",""),"mount"};QVERIFY(controller->selectReference(mount));auto apply=window->inspectorDock->findChild<QPushButton *>("applyAnchorChanges");QVERIFY(apply && !apply->isEnabled());
  auto shared=window->inspectorDock->findChild<QPushButton *>("anchorEditShared");QVERIFY(shared);QVERIFY(shared->text().contains("2 uses"));QTest::mouseClick(shared,Qt::LeftButton);
  auto label=window->inspectorDock->findChild<QLineEdit *>("anchorLabel");auto z=window->inspectorDock->findChild<QDoubleSpinBox *>("anchorTranslation2");QVERIFY(label && z);label->setText("Shared mount");z->setValue(2);apply=window->inspectorDock->findChild<QPushButton *>("applyAnchorChanges");QVERIFY(apply->isEnabled());QTest::mouseClick(apply,Qt::LeftButton);auto edited=window->activeEditor->toPlainText();QVERIFY(edited.contains("width/2"));QVERIFY(edited!=source);evaluate(edited);if(QTest::currentTestFailed())return;
  QCOMPARE(controller->resolve(mount).declaration->label,std::string("Shared mount"));QVERIFY(controller->resolve(mount).frame->translation().isApprox(Vector3d(20,0,12)));Anchors::ReferenceKey other{Anchors::instanceOwner("/fixture-b/plate",""),"mount"};QCOMPARE(controller->resolve(other).declaration->label,std::string("Shared mount"));
  window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);evaluate(source);if(QTest::currentTestFailed())return;
  Anchors::ReferenceKey relation{Anchors::instanceOwner("/fixture-a",""),"peg-on-plate"};QVERIFY(controller->selectReference(relation));shared=window->inspectorDock->findChild<QPushButton *>("anchorEditShared");QVERIFY(shared);QTest::mouseClick(shared,Qt::LeftButton);
  auto offset=window->inspectorDock->findChild<QDoubleSpinBox *>("attachmentOffset2");auto spin=window->inspectorDock->findChild<QDoubleSpinBox *>("attachmentSpin");auto mode=window->inspectorDock->findChild<QComboBox *>("attachmentMode");QVERIFY(offset && spin && mode);offset->setValue(5);spin->setValue(15);mode->setCurrentIndex(1);apply=window->inspectorDock->findChild<QPushButton *>("applyAnchorChanges");QVERIFY(apply->isEnabled());QTest::mouseClick(apply,Qt::LeftButton);edited=window->activeEditor->toPlainText();QVERIFY(edited.contains("spin: 0"));QVERIFY(edited.contains("offset: [0,0,0]"));evaluate(edited);if(QTest::currentTestFailed())return;
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());auto relations=Anchors::referenceScene(*scene->semantic);QCOMPARE(relations->attachments.at(relation).value.mode,Anchors::AttachmentMode::Align);QCOMPARE(relations->attachments.at(relation).value.spinAdjustment,15.);QVERIFY(scene->semantic->instances.at("/fixture-a/peg").localToWorld.translation().isApprox(Vector3d(20,0,15)));QVERIFY(scene->semantic->instances.at("/fixture-b/peg").localToWorld.translation().isApprox(Vector3d(130,0,15)));
}
void TestAnchors::sourceOwnerReadOnlyAndConcurrentGuards(){
  auto source=fixture();evaluate(source);if(QTest::currentTestFailed())return;auto controller=window->anchorController;auto editor=dynamic_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(editor);Anchors::ReferenceKey key{Anchors::instanceOwner("/fixture-a/plate",""),"mount"};QVERIFY(controller->allowSharedEdit(key));
  auto literal=std::make_shared<const Value>(Value("Changed"));auto stamp=window->evaluationBinding->stamp();auto prepared=controller->prepareEdit(key,{{"label",literal}},stamp);QVERIFY(prepared);
  editor->qsci->setReadOnly(true);QVERIFY(!controller->prepareEdit(key,{{"label",literal}},stamp));QCOMPARE(SourceEditing::apply(*prepared.editor,prepared.request).error,SourceEditing::EditError::ReadOnly);QCOMPARE(editor->toPlainText(),source);editor->qsci->setReadOnly(false);
  editor->insertOrReplaceText("// concurrent edit\n");auto concurrent=editor->toPlainText();QCOMPARE(SourceEditing::apply(*prepared.editor,prepared.request).error,SourceEditing::EditError::StaleSource);QVERIFY(!controller->editReference(key,{{"label",literal}}));QCOMPARE(editor->toPlainText(),concurrent);
}
void TestAnchors::affineCaptureAndTypedHelpers(){
  evaluate("@{type:Part,id:\"piece\"}module piece(){cube(10);}rotate([35,20,15])scale([-2,3,.5])@{type:Component,id:\"piece\"}piece();\n");if(QTest::currentTestFailed())return;
  auto controller=window->anchorController;auto view=window->qglview;auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());const auto placement=scene->semantic->instances.at("/piece").localToWorld;
  const Vector3d point=placement*Vector3d(5,5,10),normal=(placement.linear().inverse().transpose()*Vector3d::UnitZ()).normalized();
  view->navigation()->apply({CameraPose::upright(normal),-point,100,45,Camera::ProjectionType::ORTHOGONAL});view->update();QTest::qWait(20);
  auto hit=window->selectionService->pick(projected(*view,point),window->evaluationBinding->stamp());QVERIFY2(hit.hit,qPrintable(hit.detail));QVERIFY(hit.hit->outwardNormal.dot(normal)>.999);
  std::string error;QVERIFY2(controller->capture(*hit.hit,{},&error),qPrintable(QString::fromStdString(error)));auto draft=*controller->captureDraft();auto key=*Anchors::referenceKey(draft.key);
  const auto anchor=std::get<Anchors::Anchor>(std::dynamic_pointer_cast<const Anchors::ReferenceDeclaration>(draft.preview->evaluated)->value);auto transformed=Anchors::transformReference(anchor,placement);QVERIFY(transformed.frame);QVERIFY(transformed.reflectedOwner);QVERIFY(transformed.frame->translation().isApprox(hit.hit->worldPoint,1e-10));QVERIFY(transformed.frame->linear().col(2).isApprox(hit.hit->outwardNormal,1e-10));
  QVERIFY(controller->saveCapture());auto saved=window->activeEditor->toPlainText();view->navigation()->orient({60,0,40});evaluate(saved);if(QTest::currentTestFailed())return;auto resolved=controller->resolve(key);QVERIFY(resolved.frame);QVERIFY(resolved.frame->matrix().isApprox(transformed.frame->matrix(),1e-10));
  const auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");window->inspectorDock->show();window->inspectorDock->raise();QVERIFY(controller->selectReference(key));auto scroll=window->inspectorDock->findChild<QScrollArea *>();QVERIFY(scroll);scroll->ensureWidgetVisible(window->inspectorDock->findChild<QLabel *>("anchorReferenceStatus"));QTest::qWait(20);if(!output.isEmpty())QVERIFY(window->grab().save(output+"/affine-fixed-anchor.png"));
  evaluate("cube(10);@{type:Point}p={id:\"p\",position:[12,0,12]};@{type:Axis}a={id:\"a\",position:[0,12,12],direction:[1,1,0]};@{type:Plane}n={id:\"n\",position:[12,12,12],normal:[0,0,1]};\n");if(QTest::currentTestFailed())return;
  scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());auto refs=Anchors::referenceScene(*scene->semantic);auto source=window->activeEditor->toPlainText();auto renderer=view->renderer;
  for(const auto& [helper,record]:refs->references){auto typed=controller->resolve(helper);QCOMPARE(typed.state,Design::ResolutionState::Resolved);QVERIFY(!typed.frame);QVERIFY(controller->selectReference(helper));auto label=window->inspectorDock->findChild<QLabel *>("anchorReferenceStatus");QVERIFY(label);QVERIFY(label->text().contains(QString::fromStdString(record.declaration->kind)));view->update();QTest::qWait(20);if(!output.isEmpty())QVERIFY(window->grab().save(output+"/typed-"+QString::fromStdString(helper.id)+".png"));QCOMPARE(window->activeEditor->toPlainText(),source);QCOMPARE(view->renderer,renderer);}
}
void TestAnchors::includedOwnerAndRetainedResolverGuards(){
  QTemporaryDir directory;QVERIFY(directory.isValid());auto ownerPath=directory.filePath("owner.scad"),rootPath=directory.filePath("root.scad");
  const QByteArray ownerSource="@{type:Part,id:\"piece\"}module piece(){@{type:Anchor}mount={id:\"mount\",position:[0,0,1],normal:[0,0,1],up:[0,1,0]};cube(1);}\n";
  QFile file(ownerPath);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(ownerSource),ownerSource.size());file.close();
  auto root=window->activeEditor;const QString source="include <owner.scad>\n@{type:Component,id:\"piece\"}piece();\n";root->setPlainText(source);QVERIFY(window->tabManager->saveAs(root,rootPath));evaluate(source);if(QTest::currentTestFailed())return;
  auto controller=window->anchorController;Anchors::ReferenceKey key{Anchors::instanceOwner("/piece",""),"mount"};auto change=std::make_shared<const Value>(Value("Edited"));QVERIFY(!controller->prepareEdit(key,{{"label",change}},window->evaluationBinding->stamp()));
  auto retained=controller->references();auto token=Anchors::anchorToken(key);auto stamp=window->evaluationBinding->stamp();QVERIFY(retained->resolve(*token,stamp.resultId.toStdString(),stamp.sourceFingerprint.toStdString(),stamp.configurationFingerprint.toStdString()).frame);
  QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Append));QVERIFY(file.write("// changed on disk\n")>0);file.close();
  // Before the watcher event arrives, display refresh may still show the last
  // snapshot. It must not synchronously reopen all dependencies to list it.
  QVERIFY(controller->listedReferences());
  window->inspectorDock->refresh();
  QVERIFY(controller->listedReferences());
  // Action and published-resolver checks still catch the changed bytes immediately.
  QVERIFY(!window->evaluationBinding->validate(stamp));
  QVERIFY(!controller->prepareEdit(key,{{"label",change}},stamp));
  QCOMPARE(retained->resolve(*token,stamp.resultId.toStdString(),stamp.sourceFingerprint.toStdString(),stamp.configurationFingerprint.toStdString()).state,Design::ResolutionState::Changed);QCOMPARE(root->toPlainText(),source);
  window->tabManager->open(ownerPath);auto owner=window->activeEditor;QVERIFY(owner!=root);window->tabManager->open(rootPath);evaluate(source);if(QTest::currentTestFailed())return;stamp=window->evaluationBinding->stamp();auto prepared=controller->prepareEdit(key,{{"label",change}},stamp);QVERIFY2(prepared,qPrintable(QString::fromStdString(prepared.error)));QCOMPARE(prepared.editor.data(),owner);
  owner->insertOrReplaceText("// concurrent included edit\n");auto concurrent=owner->toPlainText();QVERIFY(!window->evaluationBinding->current());QCOMPARE(SourceEditing::apply(*owner,prepared.request).error,SourceEditing::EditError::StaleSource);QCOMPARE(owner->toPlainText(),concurrent);QCOMPARE(root->toPlainText(),source);
  evaluate(source);if(QTest::currentTestFailed())return;prepared=controller->prepareEdit(key,{{"label",change}},window->evaluationBinding->stamp());QVERIFY(prepared);window->tabManager->open(ownerPath);owner->setContentModified(false);window->tabManager->closeCurrentTab();QVERIFY(!window->evaluationBinding->current());QVERIFY(!controller->editReference(key,{{"label",change}}));QCOMPARE(root->toPlainText(),source);
}
void TestAnchors::sourceAnnotationAndAnchorJointUndo(){
  auto trace=[](const char *step){const auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");if(!output.isEmpty()){QFile file(output+"/joint-undo-progress.txt");if(file.open(QIODevice::Append))file.write(QByteArray(step)+"\n");}};trace("begin");
  const QString source="cube(10);\n";evaluate(source);if(QTest::currentTestFailed())return;auto editor=window->activeEditor;editor->clearUndoHistory();editor->insertOrReplaceText("// source undo\n");auto inserted=editor->toPlainText();evaluate(inserted);if(QTest::currentTestFailed())return;QVERIFY(editor->annotations->add(undoAnnotation()));trace("annotation added");
  auto view=window->qglview;const Vector3d point(5,5,10);view->navigation()->apply({CameraPose::upright(Vector3d::UnitZ()),-point,100,45,Camera::ProjectionType::ORTHOGONAL});view->update();QTest::qWait(20);
  auto hit=window->selectionService->pick(projected(*view,point),window->evaluationBinding->stamp());QVERIFY(hit.hit);auto controller=window->anchorController;QVERIFY(controller->capture(*hit.hit));auto draft=*controller->captureDraft();auto key=*Anchors::referenceKey(draft.key);QVERIFY(controller->saveCapture());auto saved=editor->toPlainText();trace("anchor saved");
  editor->undo();QCOMPARE(editor->toPlainText(),inserted);QVERIFY(controller->captureDraft());editor->undo();QVERIFY(editor->annotations->records().empty());QCOMPARE(editor->toPlainText(),inserted);editor->undo();QCOMPARE(editor->toPlainText(),source);trace("three undos");
  editor->redo();QCOMPARE(editor->toPlainText(),inserted);editor->redo();QCOMPARE(editor->annotations->records().size(),1);editor->redo();QCOMPARE(editor->toPlainText(),saved);QVERIFY(!controller->captureDraft());trace("three redos");evaluate(saved);if(QTest::currentTestFailed())return;QVERIFY(controller->resolve(key).frame);trace("anchor reevaluated");
  // Return annotation state to its initial clean value before the fixture document is closed.
  QVERIFY(editor->annotations->remove(editor->annotations->records().front().id()));QVERIFY(!editor->annotations->dirty());
  evaluate(fixture());if(QTest::currentTestFailed())return;QVERIFY(window->semanticController->select({Semantic::TargetRole::Definition,"plate"}));Anchors::ReferenceKey defaults{Anchors::instanceOwner("/plate","plate"),"mount"};QTRY_VERIFY(controller->resolve(defaults).frame);QVERIFY(controller->resolve(defaults).frame->translation().isApprox(Vector3d(20,0,10)));QVERIFY(controller->listedReferences()->defaultDefinitionId=="plate");
  trace("default resolved");
}
