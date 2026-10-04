// SPDX-License-Identifier: GPL-2.0-or-later
#include "TestContextualInsertion.h"
#include "gui/insertion/InsertionPreviewProcess.h"
#include "gui/insertion/InsertionController.h"
#include "core/insertion/InsertionPreviewProtocol.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/InspectionPresentationController.h"
#include "gui/QGLView.h"
#include "gui/ScintillaEditor.h"
#include "gui/parameter/ParameterWidget.h"
#include "core/SourceFile.h"
#include "glview/RenderSettings.h"
#include <QSignalSpy>
#include <QTest>
#include <QElapsedTimer>
#include "gui/selection/SelectionService.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/CameraNavigation.h"
#include "glview/CameraPose.h"
#include <QLineEdit>
#include <QListWidget>
#include <QCheckBox>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QStatusBar>
#include "gui/SavedViews.h"
#include "gui/annotations/AnnotationStore.h"
#include "gui/annotations/AnnotationBridge.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/source/SourceEditTransaction.h"
#include "core/parsersettings.h"
#include "gui/semantic/SemanticMeshJob.h"
#include <QDateTime>
#include "core/Tree.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySetUtils.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUuid>
#include <QMessageBox>
#include <array>
#include <algorithm>
namespace {
using namespace Insertion;
void traceEvent(const QString& event){
  QFile file(qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT")+"/insertion-events.log");
  if(!file.open(QIODevice::Append))return;
  auto focus=QApplication::focusWidget();QString ancestors;
  for(auto widget=focus;widget;widget=widget->parentWidget())ancestors+=QString::fromLatin1(widget->metaObject()->className())+":"+widget->objectName()+"/";
  file.write(QString("%1 %2:%3 %4 focus=%5\n").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),QString::fromLatin1(QTest::currentTestFunction()),QString::fromLatin1(QTest::currentDataTag()),event,ancestors).toUtf8());
}
PreviewRequest request(MainWindow *window,const std::string& candidate){
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());if(!scene || !scene->backing)throw ProtocolError("Native insertion test requires a retained evaluation");
  PreviewRequest r;r.serial=1;r.build=executableIdentity();r.rootKey=r.targetKey=scene->inputs->root().fileKey;r.directory=EvaluationInputs::fileKey(scene->backing->parsedRoot->modulePath());r.sources=scene->inputs->consumed();r.candidate=candidate;r.candidateHash=EvaluationInputs::fingerprint(candidate);r.edits={{0,scene->inputs->root().utf8.size(),candidate}};r.configuration=captureConfiguration(*scene->backing->builtin,window->activeEditor->parameterWidget->evaluationValues());r.configurationHash=configurationHash(r.configuration);return r;
}
std::unique_ptr<PreviewProcess> job(MainWindow *window,PreviewRequest r,QObject *owner,PreviewProcess::Completion callback){
  const auto stamp=window->evaluationBinding->stamp();QPointer<MainWindow> pointer(window);
  return std::make_unique<PreviewProcess>(std::move(r),owner,[pointer,stamp]{return pointer && pointer->evaluationBinding->validateModel(stamp);},[pointer](const std::string& key){if(!pointer)throw ProtocolError("Window closed");auto inputs=Selection::EvaluationBinding::capture(*pointer,*pointer->activeEditor);return inputs->read(std::filesystem::u8path(key));},std::move(callback));
}
QPointF projected(QGLView *view,Vector3d point){view->makeCurrent();view->setupCamera();const Eigen::Map<const Eigen::Matrix4d> mv(view->modelview),projection(view->projection);Eigen::Vector4d p=projection*mv*Eigen::Vector4d(point.x(),point.y(),point.z(),1);p/=p.w();view->doneCurrent();return {((p.x()+1)*view->cam.pixel_width/2)/view->devicePixelRatio(),((1-p.y())*view->cam.pixel_height/2)/view->devicePixelRatio()};}
std::optional<Selection::Hit> picked(MainWindow *window,Vector3d point={5,5,10}){auto view=window->qglview;view->navigation()->apply({CameraPose::upright(Vector3d::UnitZ()),-point,80,45,Camera::ProjectionType::ORTHOGONAL});view->update();QTest::qWait(30);auto result=window->selectionService->pick(projected(view,point),window->evaluationBinding->stamp());if(!result.hit)qWarning("Pick fixture: %s",qPrintable(result.detail));return result.hit;}
bool choose(MainWindow *window,const std::string& id){auto controller=window->insertionController;if(!controller->choose(id))return false;for(size_t i=0;i<controller->targets().size();++i)if(controller->targets()[i].target)return controller->selectTarget(i);return false;}
BoundingBox candidateBounds(const PreviewResponse& response){BoundingBox box;for(const auto& piece:response.meshes->pieces)for(const auto& vertex:piece.mesh->vertices)box.extend(piece.localToWorld*vertex);return box;}
QString meshSignature(const InspectionMeshSet& meshes){
  std::vector<std::string> faces;
  for(const auto& piece:meshes.pieces)for(const auto& face:piece.mesh->indices){
    std::vector<std::array<long long,3>> vertices;
    for(auto index:face){const Vector3d world=piece.localToWorld*piece.mesh->vertices.at(index);vertices.push_back({std::llround(world.x()*1e6),std::llround(world.y()*1e6),std::llround(world.z()*1e6)});}
    std::sort(vertices.begin(),vertices.end());std::string text=std::to_string(int(piece.role))+":";
    for(const auto& vertex:vertices)for(auto coordinate:vertex)text+=std::to_string(coordinate)+",";
    faces.push_back(std::move(text));
  }
  std::sort(faces.begin(),faces.end());QCryptographicHash hash(QCryptographicHash::Sha256);for(const auto& face:faces)hash.addData(QByteArray::fromStdString(face+"\n"));return QString::fromLatin1(hash.result().toHex());
}
std::shared_ptr<const InspectionMeshSet> nativeMeshes(MainWindow *window){
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());if(!scene)return {};
  std::shared_ptr<InspectionMeshSet> out;
  if(scene->semantic && scene->semantic->marked){
    auto output=Semantic::prepareMeshes(*scene->semantic,"committed-source",1);if(!output.meshes)return {};
    out=std::make_shared<InspectionMeshSet>(*output.meshes);
  }else {
    NativeExecutionGuard execution;Tree tree(scene->root,scene->backing->parsedRoot->modulePath());GeometryEvaluator evaluator(tree);
    auto geometry=evaluator.evaluateGeometry(*scene->root,true);if(!geometry)return {};
    auto mesh=PolySetUtils::getGeometryAsPolySet(geometry);if(!mesh)return {};
    out=std::make_shared<InspectionMeshSet>();out->pieces.push_back({1,{},mesh,Transform3d::Identity(),Color4f(),InspectionPiece::Role::Solid});
  }
  for(auto& piece:out->pieces)if(!piece.mesh->isTriangular())piece.mesh=PolySetUtils::tessellate_faces(*piece.mesh);
  return out;
}
bool writeBytes(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();}
bool retainCatalogProof(QString item,const QString& original,const std::string& candidate,const PreviewResponse& proof){
  item.replace(':','-');const auto folder=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT")+"/catalog-fixtures/"+item;if(!QDir().mkpath(folder))return false;
  const auto box=candidateBounds(proof);auto coordinates=[](Vector3d p){return QJsonArray{p.x(),p.y(),p.z()};};QJsonArray volumes;for(const auto& value:proof.volumes)volumes.append(QJsonObject{{"path",QString::fromStdString(value.path)},{"before",value.before},{"after",value.after}});
  QJsonObject metadata{{"item",item},{"candidateSHA256",QString::fromStdString(proof.candidateHash)},{"configurationSHA256",QString::fromStdString(proof.configurationHash)},{"meshSHA256",meshSignature(*proof.meshes)},{"bounds",QJsonArray{coordinates(box.min()),coordinates(box.max())}},{"volumes",volumes}};
  return writeBytes(folder+"/original.scad",original.toUtf8()) && writeBytes(folder+"/committed.scad",QByteArray::fromStdString(candidate)) && writeBytes(folder+"/proof.cbor",QCborValue(encodeResponse(proof)).toCbor()) && writeBytes(folder+"/proof.json",QJsonDocument(metadata).toJson());
}
struct LibraryPathsRestore {std::vector<std::string> paths=get_library_path();~LibraryPathsRestore(){set_library_path(paths);}};
QJsonObject http(const QJsonObject& info,const QString& route,const QJsonObject& body={}){
  QNetworkAccessManager network;QNetworkRequest request(QUrl(info.value("url").toString()+route));
  request.setRawHeader("X-OpenSCAD-Agent","1");request.setRawHeader("X-OpenSCAD-Agent-Server",info.value("server_id").toString().toUtf8());request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
  auto reply=body.isEmpty()?network.get(request):network.post(request,QJsonDocument(body).toJson());QEventLoop loop;QTimer timeout;timeout.setSingleShot(true);timeout.start(6000);
  QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);loop.exec();
  auto result=QJsonDocument::fromJson(reply->readAll()).object();result["http_status"]=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();return result;
}
}
void TestContextualInsertion::initTestCase(){previousBackend=int(RenderSettings::inst()->backend3D);RenderSettings::inst()->backend3D=RenderBackend3D::ManifoldBackend;window->designActionAutoReload->setChecked(false);window->hide();window->show();window->raise();window->activateWindow();QVERIFY(QTest::qWaitForWindowExposed(window));QTRY_VERIFY(window->qglview->isValid());auto errors=new QTimer(this);errors->setInterval(20);connect(errors,&QTimer::timeout,this,[this]{auto dialog=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());if(!dialog || dialog->windowTitle()!="Critical Error")return;criticalErrors.append(dialog->text());qWarning("Native event exception: %s",qPrintable(dialog->text()));dialog->accept();});errors->start();
  connect(qApp,&QApplication::focusChanged,this,[this](QWidget *previous,QWidget *next){
    traceEvent("focusChanged");
    // Desktop host deactivation is outside this native fixture; intentional editor transfers stay untouched.
    if(!next && previous && previous->window()==window && !QApplication::activeModalWidget() && !window->isActiveWindow()){
      QApplication::setActiveWindow(window);previous->setFocus(Qt::OtherFocusReason);
    }
  });
  connect(window->insertionController,&Controller::changed,this,[this]{traceEvent(QString("state=%1 %2").arg(int(window->insertionController->state())).arg(window->insertionController->message()));});
  connect(window->evaluationBinding,&Selection::EvaluationBinding::invalidated,this,[]{traceEvent("evaluationInvalidated");});
  connect(window->inspectionPresentation,&InspectionPresentationController::draftDisplayChanged,this,[](bool active,bool pixels){traceEvent(QString("draft active=%1 pixels=%2").arg(active).arg(pixels));});
}
void TestContextualInsertion::init(){QApplication::setActiveWindow(window);originalTabs=window->tabManager->editorList;window->tabManager->actionNew();window->activeEditor->setPlainText("cube(10);\n");QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);QVERIFY(window->evaluationBinding->current());window->activeEditor->clearUndoHistory();traceConnection=connect(window->insertionController,&Controller::changed,this,[this]{QFile trace(qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT")+"/insertion-trace.log");if(trace.open(QIODevice::Append)){auto controller=window->insertionController;trace.write(QString("%1: %2 / %3\n").arg(int(controller->state())).arg(controller->message(),window->statusBar()->currentMessage()).toUtf8());}});}
void TestContextualInsertion::cleanup(){disconnect(traceConnection);window->insertionController->cancel();window->inspectionPresentation->cancelDraft();while(window->tabManager->editorList.size()>originalTabs.size()){while(originalTabs.contains(window->activeEditor))window->tabManager->nextTab();SavedViews::forEditor(window->activeEditor)->load(window->activeEditor->filepath);window->activeEditor->annotations->load(window->activeEditor->filepath+".annotations");window->activeEditor->parameterWidget->setModified(false);window->activeEditor->setContentModified(false);window->tabManager->closeCurrentTab();}fixtureFolder.reset();}
void TestContextualInsertion::cleanupTestCase(){RenderSettings::inst()->backend3D=RenderBackend3D(previousBackend);QVERIFY2(criticalErrors.empty(),qPrintable(criticalErrors.join("\n")));}
void TestContextualInsertion::isolatedCandidatePreservesLiveBase(){
  auto editor=window->activeEditor;auto bytes=editor->toPlainText();auto revision=editor->sourceRevision;auto stamp=window->evaluationBinding->stamp();auto scene=window->evaluationBinding->scene(stamp);auto renderer=window->qglview->renderer;QObject owner;std::optional<PreviewResponse> result;
  auto process=job(window,request(window,"difference(){cube(10);translate([2,2,-1])cube([6,6,12]);}\n"),&owner,[&](auto response){result=std::move(response);});process->start();QTRY_VERIFY_WITH_TIMEOUT(result.has_value(),15000);for(const auto& d:result->diagnostics)qInfo("%s",d.message.c_str());QVERIFY(result->status==PreviewStatus::Success);QVERIFY(result->meshes);QVERIFY(!result->empty);
  QCOMPARE(editor->toPlainText(),bytes);QCOMPARE(editor->sourceRevision,revision);QVERIFY(!editor->canUndo());QCOMPARE(window->evaluationBinding->stamp(),stamp);QCOMPARE(window->evaluationBinding->scene(stamp),scene);QCOMPARE(window->qglview->renderer,renderer);QVERIFY(!window->evaluationBinding->draftPixels());
}
void TestContextualInsertion::invalidCandidatePreservesLiveBase(){
  auto editor=window->activeEditor;auto stamp=window->evaluationBinding->stamp();auto revision=editor->sourceRevision;QObject owner;std::optional<PreviewResponse> result;auto process=job(window,request(window,"cube([);"),&owner,[&](auto response){result=std::move(response);});process->start();QTRY_VERIFY_WITH_TIMEOUT(result.has_value(),15000);QVERIFY(result->status==PreviewStatus::Invalid);QVERIFY(!result->meshes);QCOMPARE(editor->sourceRevision,revision);QCOMPARE(editor->toPlainText(),QString("cube(10);\n"));QVERIFY(!editor->canUndo());QVERIFY(window->evaluationBinding->validateModel(stamp));
}
void TestContextualInsertion::cancellationDuringGeometryIsAsynchronous(){
  auto stamp=window->evaluationBinding->stamp();QObject owner;int callbacks=0;auto process=job(window,request(window,"minkowski(){sphere(r=50,$fn=1000);sphere(r=20,$fn=1000);}"),&owner,[&](auto){++callbacks;});process->start();QTest::qWait(500);QVERIFY(process->running());QElapsedTimer clock;clock.start();process->cancel();QVERIFY(clock.elapsed()<250);QTest::qWait(100);QCOMPARE(callbacks,0);QVERIFY(!process->running());QVERIFY(window->evaluationBinding->validateModel(stamp));QVERIFY(!window->activeEditor->canUndo());
}
void TestContextualInsertion::changedSourceRejectsLateDelivery(){
  QObject owner;std::optional<PreviewResponse> result;auto process=job(window,request(window,"cube(20);"),&owner,[&](auto response){result=std::move(response);});process->start();window->activeEditor->setPlainText("cube(11);\n");QTRY_VERIFY_WITH_TIMEOUT(result.has_value(),10000);QVERIFY(result->status==PreviewStatus::Invalid);QVERIFY(!result->meshes);QCOMPARE(window->activeEditor->toPlainText(),QString("cube(11);\n"));QVERIFY(!window->evaluationBinding->current());
}
void TestContextualInsertion::ownerTeardownCancelsOnce(){
  auto stamp=window->evaluationBinding->stamp();int callbacks=0;auto owner=std::make_unique<QObject>();auto process=new PreviewProcess(request(window,"minkowski(){sphere(r=50,$fn=1000);sphere(r=20,$fn=1000);}"),owner.get(),[this,stamp]{return window->evaluationBinding->validateModel(stamp);},{},[&](auto){++callbacks;});QPointer<PreviewProcess> pointer(process);process->start();QTest::qWait(20);QElapsedTimer clock;clock.start();owner.reset();QVERIFY(clock.elapsed()<250);QVERIFY(!pointer);QTest::qWait(100);QCOMPARE(callbacks,0);QVERIFY(window->evaluationBinding->validateModel(stamp));QVERIFY(!window->activeEditor->canUndo());
}
void TestContextualInsertion::paletteFiltersMetadataAndUnavailableItems(){
  auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));auto list=window->findChild<QListWidget *>("insertionPalette");auto search=window->findChild<QLineEdit *>("insertionSearch");QVERIFY(list && search);QCOMPARE(controller->catalogs()->items.size(),size_t(8));QCOMPARE(list->count(),8);search->setText("plain-hole");QCOMPARE(list->count(),1);QCOMPARE(list->item(0)->data(Qt::UserRole).toString(),QString("scadmaxx.builtin:plain-hole"));search->clear();QCOMPARE(list->count(),8);QVERIFY(!window->activeEditor->canUndo());controller->reloadProviders();QCOMPARE(controller->state(),Controller::State::Idle);QVERIFY(!window->inspectionPresentation->draftActive());
}
void TestContextualInsertion::actualCutPreviewCancelsWithoutSourceOrUndo(){
  auto hit=picked(window);QVERIFY(hit);auto editor=window->activeEditor;auto bytes=editor->toPlainText();auto revision=editor->sourceRevision;auto base=window->evaluationBinding->stamp();auto view=window->qglview;auto camera=view->navigation()->current();auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:plain-hole"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);QVERIFY(controller->proof());QVERIFY(controller->proof()->volumes[0].after<controller->proof()->volumes[0].before);QVERIFY(candidateBounds(*controller->proof()).max().isApprox(Vector3d(10,10,10)));QCOMPARE(editor->toPlainText(),bytes);QCOMPARE(editor->sourceRevision,revision);QVERIFY(!editor->canUndo());QVERIFY(window->evaluationBinding->draftPixels());QVERIFY(window->evaluationBinding->validateModel(base));QCOMPARE(window->selectionService->pick({10,10},window->evaluationBinding->stamp()).error,Selection::PickError::Unsupported);QVERIFY(view->navigation()->current()==camera);view->setFocus();QTest::keyClick(view,Qt::Key_Escape);QTRY_COMPARE(controller->state(),Controller::State::Idle);QVERIFY(!window->evaluationBinding->draftPixels());QVERIFY(!window->inspectionPresentation->draftActive());QCOMPARE(editor->toPlainText(),bytes);QVERIFY(!editor->canUndo());QVERIFY(window->evaluationBinding->validate(window->evaluationBinding->stamp()));
}
void TestContextualInsertion::invalidFieldsAndRapidChangesKeepOnlyLatestCandidate(){
  auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:plain-hole"));auto diameter=window->findChild<QLineEdit *>("insertionParameter_diameter");QVERIFY(diameter);diameter->setText("nan");QCOMPARE(controller->state(),Controller::State::Configuring);QVERIFY(!controller->proof());QTest::qWait(200);QVERIFY(!window->activeEditor->canUndo());for(int value=3;value<9;++value)diameter->setText(QString::number(value));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);QVERIFY(controller->editPlan()->candidate.find("diameter = 8")!=std::string::npos || controller->editPlan()->candidate.find("d = 8")!=std::string::npos);QCOMPARE(controller->proof()->candidateHash,controller->editPlan()->candidateHash);QVERIFY(!window->activeEditor->canUndo());controller->setPlacement({100,100,0},0,false);QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Configuring,15000);QVERIFY(controller->message().contains("NoIntersection") || controller->message().contains("remove volume"));QVERIFY(!controller->proof());
}
void TestContextualInsertion::groupedFocusAndEditorShortcutsStaySeparate(){
  auto hit=picked(window);QVERIFY(hit);auto view=window->qglview;view->setFocus();QTest::keyClick(view,Qt::Key_A,Qt::ShiftModifier);auto controller=window->insertionController;QTRY_COMPARE(controller->state(),Controller::State::Choosing);QVERIFY(choose(window,"scadmaxx.builtin:cube"));auto field=window->findChild<QLineEdit *>("insertionParameter_width");QVERIFY(field);field->setFocus();QTest::qWait(20);QVERIFY(window->viewportTools->active());view->setFocus();QTest::qWait(20);QVERIFY(window->viewportTools->active());window->activeEditor->setFocus();QTRY_COMPARE(controller->state(),Controller::State::Idle);QTest::keyClick(window->activeEditor,Qt::Key_A,Qt::ShiftModifier);QTest::qWait(20);QCOMPARE(controller->state(),Controller::State::Idle);
}
void TestContextualInsertion::numericFieldsAcceptBeforeViewportCommit(){
  auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);auto field=window->findChild<QLineEdit *>("insertionParameter_width");QVERIFY(field);field->setFocus();QTest::keyClick(field,Qt::Key_Return);QTest::qWait(30);QCOMPARE(controller->state(),Controller::State::Ready);QVERIFY(!window->activeEditor->canUndo());QCOMPARE(window->activeEditor->toPlainText(),QString("cube(10);\n"));
}
void TestContextualInsertion::realPlaneMoveSpinFlipAndNavigation(){
  auto hit=picked(window);QVERIFY(hit);auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());
  auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));
  QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);
  auto frame=controller->draftPlacement()->movementFrame(controller->editPlan()->target,scene->semantic);QVERIFY(frame);
  QVERIFY(controller->setPlacement({0,0,2},0,false));auto view=window->qglview;view->setFocus();QTest::keyClick(view,Qt::Key_G);
  const auto first=projected(view,{6,5,10}),second=projected(view,{8,7,10});
  QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,first.toPoint());QTest::mouseMove(view,second.toPoint());QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,second.toPoint());
  QVERIFY(controller->draftPlacement());QVERIFY(std::abs(controller->draftPlacement()->offset.z()-2)<1e-10);
  QVERIFY(controller->draftPlacement()->offset.head<2>().norm()>1);
  QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);
  const auto moved=*controller->draftPlacement();
  const auto spinStart=projected(view,*frame.value*Vector3d(moved.offset.x()+15,moved.offset.y(),0));
  const auto spinEnd=projected(view,*frame.value*Vector3d(moved.offset.x(),moved.offset.y()+15,0));
  view->setFocus();QTest::qWait(30);QTest::keyClick(view,Qt::Key_R);QTest::qWait(30);QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,spinStart.toPoint());
  QTest::mouseMove(view,spinEnd.toPoint());QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,spinEnd.toPoint());
  QVERIFY2(std::abs(controller->draftPlacement()->spinDegrees-90)<1.5,qPrintable(QString("Actual spin: %1").arg(controller->draftPlacement()->spinDegrees)));QVERIFY(controller->draftPlacement()->offset==moved.offset);
  QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);
  auto pose=view->navigation()->current();auto translated=pose;translated.translation+=Vector3d(1,2,0);
  view->navigation()->apply(translated);QTest::qWait(300);QCOMPARE(controller->state(),Controller::State::Ready);
  const auto before=*controller->draftPlacement();QTest::keyClick(view,Qt::Key_F);
  QVERIFY(controller->draftPlacement()->flipped!=before.flipped);QVERIFY(controller->draftPlacement()->offset==before.offset);
  QVERIFY(!controller->proof());QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);
  QVERIFY(view->navigation()->current()==translated);QVERIFY(!window->activeEditor->canUndo());
}
void TestContextualInsertion::sharedScopeAndNamedAnchorRemainExplicit(){
  window->activeEditor->setPlainText("@{type: Part,id:\"plate\"}module plate(width=20){@{type: Anchor}mount={id:\"mount\",position:[width/2,10,10],normal:[0,0,1],up:[0,1,0]};cube([width,20,10]);}\n@{type: Assembly,id:\"fixture\"}module fixture(width=20){@{type: Component,id:\"plate\"}plate(width=width);}\n@{type: Component,id:\"fixture-a\"}fixture(width=20);translate([50,0,0]){@{type: Component,id:\"fixture-b\"}fixture(width=30);}");QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);QVERIFY(window->evaluationBinding->current());QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==InspectionPresentationController::Status::Ready,15000);window->activeEditor->clearUndoHistory();auto hit=picked(window,{10,10,10});QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:plain-hole"));QCOMPARE(controller->state(),Controller::State::Configuring);QVERIFY(!controller->proof());auto shared=window->findChild<QCheckBox *>("insertionSharedScope");QVERIFY(shared && shared->isVisible());QVERIFY(controller->targets()[0].target->affectedPaths.size()==2);controller->approveShared(true);QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);QCOMPARE(controller->proof()->volumes.size(),size_t(2));QVERIFY(choose(window,"scadmaxx.bolts:iso7089"));QVERIFY(controller->setNamedAnchor(Anchors::ReferenceKey{{Anchors::OwnerRole::Instance,"/fixture-a/plate"},"mount"}));controller->approveShared(true);QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,30000);QVERIFY(controller->editPlan()->candidate.find("target: {component: \"plate\", anchor: \"mount\"}")!=std::string::npos);QVERIFY(!window->activeEditor->canUndo());
}
void TestContextualInsertion::allCatalogCommitUndoRedoSaveReopen_data(){QTest::addColumn<QString>("item");for(const char *id:{"scadmaxx.builtin:cube","scadmaxx.builtin:sphere","scadmaxx.builtin:cylinder","scadmaxx.builtin:plain-hole","scadmaxx.bosl2:threaded-hole","scadmaxx.bolts:iso4017","scadmaxx.bolts:iso4032","scadmaxx.bolts:iso7089"})QTest::newRow(id)<<QString::fromLatin1(id);}
void TestContextualInsertion::allCatalogCommitUndoRedoSaveReopen(){
  QFETCH(QString,item);
  fixtureFolder=std::make_unique<QTemporaryDir>();auto& folder=*fixtureFolder;QVERIFY(folder.isValid());
  const auto path=folder.path()+"/commit source 測試.scad";
  const QString original="$fn=48;\n// Preserve this source comment.\ncube([20,20,10]);\n";
  auto editor=window->activeEditor;editor->setPlainText(original);QVERIFY(window->tabManager->saveAs(editor,path));
  QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();
  QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);editor->clearUndoHistory();
  auto hit=picked(window,{10,10,10});QVERIFY(hit);auto controller=window->insertionController;
  QVERIFY(controller->begin(*hit));QVERIFY(choose(window,item.toStdString()));
  if(!item.contains("builtin"))QVERIFY(controller->setValues({{"size",std::string("M4")}}));
  QTRY_VERIFY2_WITH_TIMEOUT(controller->state()==Controller::State::Ready,qPrintable(controller->message()),45000);
  const auto candidate=controller->editPlan()->candidate;const auto proof=*controller->proof();
  const auto expected=candidateBounds(proof);const auto signature=meshSignature(*proof.meshes);
  QVERIFY(retainCatalogProof(item,original,candidate,proof));
  QFile disk(path);QVERIFY(disk.open(QIODevice::ReadOnly));const auto originalDisk=disk.readAll();
  QCOMPARE(QByteArray(originalDisk).replace("\r\n","\n"),original.toUtf8());disk.close();
  done.clear();QVERIFY(controller->commit());QCOMPARE(editor->toPlainText().toUtf8().toStdString(),candidate);
  QCOMPARE(controller->state(),Controller::State::Idle);
  QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),45000);QVERIFY(window->evaluationBinding->current());
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());
  QCOMPARE(configurationHash(captureConfiguration(*scene->backing->builtin,editor->parameterWidget->evaluationValues())),proof.configurationHash);
  auto meshes=nativeMeshes(window);QVERIFY(meshes);QCOMPARE(meshSignature(*meshes),signature);
  QVERIFY(window->qglview->renderer->getBoundingBox().min().isApprox(expected.min(),1e-6));
  QVERIFY(window->qglview->renderer->getBoundingBox().max().isApprox(expected.max(),1e-6));
  QVERIFY(disk.open(QIODevice::ReadOnly));QCOMPARE(disk.readAll(),originalDisk);disk.close();
  editor->undo();QCOMPARE(editor->toPlainText(),original);QVERIFY(!editor->canUndo());
  editor->redo();QCOMPARE(editor->toPlainText().toUtf8().toStdString(),candidate);
  QVERIFY(disk.open(QIODevice::ReadOnly));QCOMPARE(disk.readAll(),originalDisk);disk.close();
  QVERIFY(window->tabManager->save(editor));QVERIFY(disk.open(QIODevice::ReadOnly));
  QCOMPARE(disk.readAll().replace("\r\n","\n").toStdString(),candidate);disk.close();
  const auto independent=folder.path()+"/independent reopened.scad";QVERIFY(QFile::copy(path,independent));
  window->tabManager->createTab(independent);QVERIFY(window->activeEditor!=editor);
  QCOMPARE(window->activeEditor->toPlainText().toUtf8().toStdString(),candidate);
  done.clear();window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),45000);
  QVERIFY(window->evaluationBinding->current());
  meshes=nativeMeshes(window);QVERIFY(meshes);QCOMPARE(meshSignature(*meshes),signature);
  window->activeEditor->setContentModified(false);window->activeEditor->parameterWidget->setModified(false);
  window->tabManager->closeCurrentTab();QCOMPARE(window->activeEditor,editor);editor->parameterWidget->setModified(false);
}
void TestContextualInsertion::includedTargetCommitsExactlyItsCapturedEditor(){
  fixtureFolder=std::make_unique<QTemporaryDir>();auto& folder=*fixtureFolder;QVERIFY(folder.isValid());const auto includedPath=folder.path()+"/included part.scad",rootPath=folder.path()+"/root.scad";const QByteArray part="@{type: Part,id:\"plate\"}module plate(){cube([20,20,10]);}\n";QFile file(includedPath);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(part),qint64(part.size()));file.close();auto root=window->activeEditor;root->setPlainText("include <included part.scad>\ntranslate([50,0,0]){@{type: Component,id:\"plate\"}plate();}\n");QVERIFY(window->tabManager->saveAs(root,rootPath));const auto rootBytes=root->toPlainText();window->tabManager->createTab(includedPath);auto included=window->activeEditor;QVERIFY(included!=root);window->tabManager->open(rootPath);QCOMPARE(window->activeEditor,root);QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);QTRY_COMPARE(window->inspectionPresentation->status(),InspectionPresentationController::Status::Ready);root->clearUndoHistory();included->clearUndoHistory();auto hit=picked(window,{60,10,10});QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:plain-hole"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);QCOMPARE(controller->editPlan()->target.body.fileKey,EvaluationInputs::fileKey(std::filesystem::u8path(includedPath.toUtf8().toStdString())));const auto candidate=controller->editPlan()->candidate;done.clear();QVERIFY(controller->commit());QCOMPARE(root->toPlainText(),rootBytes);QVERIFY(!root->canUndo());QCOMPARE(included->toPlainText().toUtf8().toStdString(),candidate);QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);QVERIFY(window->evaluationBinding->current());QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),part);file.close();included->undo();QCOMPARE(included->toPlainText().toUtf8(),part);included->redo();QCOMPARE(included->toPlainText().toUtf8().toStdString(),candidate);included->setContentModified(false);included->parameterWidget->setModified(false);window->tabManager->open(includedPath);window->tabManager->closeCurrentTab();QCOMPARE(window->activeEditor,root);
}
void TestContextualInsertion::readonlyAndSynchronousPreEditChangesRejectCommit(){
  auto hit=picked(window);QVERIFY(hit);auto editor=dynamic_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(editor);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);editor->qsci->setReadOnly(true);QVERIFY(!controller->commit());QCOMPARE(editor->toPlainText(),QString("cube(10);\n"));QVERIFY(!editor->canUndo());editor->qsci->setReadOnly(false);
  hit=picked(window);QVERIFY(hit);QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);bool observed=false;auto hook=connect(editor,&EditorInterface::externalEditStarting,this,[editor,&observed]{if(observed)return;observed=true;editor->setPlainText("cube(11); // Observer edit\n");});QVERIFY(!controller->commit());disconnect(hook);QVERIFY(observed);QCOMPARE(editor->toPlainText(),QString("cube(11); // Observer edit\n"));QVERIFY(!editor->canUndo());QVERIFY(!window->inspectionPresentation->draftActive());
}
void TestContextualInsertion::sourceAndTabChangesCancelReadyCommit(){
  auto hit=picked(window);QVERIFY(hit);auto root=window->activeEditor;auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);window->tabManager->actionNew();QCOMPARE(controller->state(),Controller::State::Idle);QVERIFY(!controller->commit());QCOMPARE(root->toPlainText(),QString("cube(10);\n"));QVERIFY(!root->canUndo());window->activeEditor->setContentModified(false);window->tabManager->closeCurrentTab();QCOMPARE(window->activeEditor,root);QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);hit=picked(window);QVERIFY(hit);QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);root->setPlainText("cube(12);\n");QCOMPARE(controller->state(),Controller::State::Idle);QVERIFY(!controller->commit());QCOMPARE(root->toPlainText(),QString("cube(12);\n"));QVERIFY(!root->canUndo());
}
void TestContextualInsertion::precedingTypingHasItsOwnUndoGroup(){
  auto editor=dynamic_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(editor);const auto original=editor->toPlainText();
  editor->qsci->setCursorPosition(1,0);QTest::keyClicks(editor->qsci,"// Earlier typing");const auto typed=editor->toPlainText();QVERIFY(typed!=original);
  QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);
  auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));
  QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);const auto candidate=controller->editPlan()->candidate;
  QVERIFY(controller->commit());editor->undo();QCOMPARE(editor->toPlainText(),typed);QVERIFY(editor->canUndo());
  editor->undo();QCOMPARE(editor->toPlainText(),original);editor->redo();QCOMPARE(editor->toPlainText(),typed);
  editor->redo();QCOMPARE(editor->toPlainText().toUtf8().toStdString(),candidate);
  QTRY_VERIFY_WITH_TIMEOUT(!GuiLocker::isLocked(),20000);
}
void TestContextualInsertion::postCommitEvaluationFailureRetainsSourceAndUndo(){
  fixtureFolder=std::make_unique<QTemporaryDir>();QVERIFY(fixtureFolder->isValid());const auto dependency=fixtureFolder->path()+"/values.scad";
  QVERIFY(writeBytes(dependency,"width=10;\n"));auto editor=window->activeEditor;editor->setPlainText("include <values.scad>\ncube(width);\n");
  QVERIFY(window->tabManager->saveAs(editor,fixtureFolder->path()+"/root.scad"));QSignalSpy done(window,&MainWindow::compilationDone);
  window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);editor->clearUndoHistory();const auto original=editor->toPlainText();
  auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));
  QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);const auto candidate=controller->editPlan()->candidate;
  bool changed=false;auto hook=connect(editor,&EditorInterface::contentsChanged,this,[&]{changed=writeBytes(dependency,"width=[;\n");});
  done.clear();QVERIFY(controller->commit());disconnect(hook);QVERIFY(changed);
  // Native parser failure uses exceptionCleanup(), which releases the GUI without compilationDone.
  QTRY_VERIFY_WITH_TIMEOUT(!GuiLocker::isLocked(),20000);QVERIFY(window->compileErrors>0);
  QCOMPARE(window->lastCompiledDoc.toUtf8().toStdString(),candidate);QVERIFY(!window->evaluationBinding->current());
  QCOMPARE(editor->toPlainText().toUtf8().toStdString(),candidate);QVERIFY(!window->inspectionPresentation->draftActive());
  editor->undo();QCOMPARE(editor->toPlainText(),original);QVERIFY(!editor->canUndo());
}
void TestContextualInsertion::libraryOrderAndCatalogReloadInvalidateProof(){
  LibraryPathsRestore restore;auto controller=window->insertionController;auto hit=picked(window);QVERIFY(hit);
  QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);
  auto reversed=restore.paths;std::reverse(reversed.begin(),reversed.end());QVERIFY(reversed!=restore.paths);set_library_path(reversed);
  QTRY_COMPARE(controller->state(),Controller::State::Idle);QVERIFY(!controller->commit());QVERIFY(!window->activeEditor->canUndo());
  set_library_path(restore.paths);QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);
  hit=picked(window);QVERIFY(hit);QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);
  controller->reloadProviders();QCOMPARE(controller->state(),Controller::State::Idle);QVERIFY(!controller->commit());
  QCOMPARE(window->activeEditor->toPlainText(),QString("cube(10);\n"));QVERIFY(!window->activeEditor->canUndo());
}
void TestContextualInsertion::closedIncludedTargetOpensProvenSource(){
  fixtureFolder=std::make_unique<QTemporaryDir>();QVERIFY(fixtureFolder->isValid());const auto included=fixtureFolder->path()+"/closed.scad";
  const QByteArray part="@{type: Part,id:\"plate\"}module plate(){cube(10);}\n";QVERIFY(writeBytes(included,part));auto root=window->activeEditor;
  root->setPlainText("include <closed.scad>\n@{type: Component,id:\"plate\"}plate();\n");QVERIFY(window->tabManager->saveAs(root,fixtureFolder->path()+"/root.scad"));
  QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);
  QTRY_COMPARE(window->inspectionPresentation->status(),InspectionPresentationController::Status::Ready);root->clearUndoHistory();const auto original=root->toPlainText();
  auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;QVERIFY(controller->begin(*hit));QVERIFY(controller->choose("scadmaxx.builtin:plain-hole"));
  const auto& options=controller->targets();size_t selected=options.size();for(size_t i=0;i<options.size();++i)if(options[i].diagnostic && options[i].diagnostic->code=="OpenSource")selected=i;
  QVERIFY(selected<options.size());QVERIFY(!controller->selectTarget(selected));QVERIFY(!controller->commit());QVERIFY(controller->openSource(selected));
  QCOMPARE(controller->state(),Controller::State::Idle);QCOMPARE(window->activeEditor->filepath,included);
  QCOMPARE(window->activeEditor->toPlainText().toUtf8(),part);QVERIFY(!window->activeEditor->canUndo());QCOMPARE(root->toPlainText(),original);QVERIFY(!root->canUndo());
}
void TestContextualInsertion::unavailableLibrariesRemainVisibleWithReasons(){
  LibraryPathsRestore restore;set_library_path({});QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();
  QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;
  QVERIFY(controller->begin(*hit));auto palette=window->findChild<QListWidget *>("insertionPalette");QVERIFY(palette);QCOMPARE(palette->count(),8);
  int available=0,unavailable=0;for(int i=0;i<palette->count();++i){auto row=palette->item(i);if(row->flags().testFlag(Qt::ItemIsEnabled))++available;else{++unavailable;QVERIFY(!row->toolTip().isEmpty());QVERIFY(row->text().contains(QString::fromUtf8("—")));}}
  QCOMPARE(available,4);QCOMPARE(unavailable,4);QVERIFY(!controller->choose("scadmaxx.bosl2:threaded-hole"));QVERIFY(!controller->commit());QVERIFY(!window->activeEditor->canUndo());
}
void TestContextualInsertion::httpSnapshotsAndConcurrentSourceTransaction(){
  auto editor=window->activeEditor;const auto original=editor->toPlainText();auto hit=picked(window);QVERIFY(hit);auto controller=window->insertionController;
  QVERIFY(controller->begin(*hit));QVERIFY(choose(window,"scadmaxx.builtin:cube"));QTRY_VERIFY_WITH_TIMEOUT(controller->state()==Controller::State::Ready,15000);
  auto bridge=AgentAnnotations::Bridge::instance();bridge->start(false);struct Stop{AgentAnnotations::Bridge *bridge;~Stop(){bridge->stop();}}stop{bridge};
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(),1);const auto info=bridge->connection();const auto id=editor->documentId();
  QJsonObject body{{"request_id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"server_id",info.value("server_id")},{"document_id",id},{"window_id",window->annotationController->windowId},{"origin","editor"},{"expected_source_revision",QString::number(editor->sourceRevision)}};
  QCOMPARE(http(info,"/v1/documents/"+id+"/source-snapshots",body).value("http_status").toInt(),202);QJsonObject job;
  QTRY_COMPARE((job=http(info,"/v1/requests/"+body.value("request_id").toString())).value("state").toString(),QString("succeeded"));
  auto snapshot=http(info,"/v1/source-snapshots/"+job.value("result").toObject().value("snapshot_id").toString());QCOMPARE(snapshot.value("text").toString(),original);
  QCOMPARE(controller->state(),Controller::State::Ready);
  // The existing API is read-only for source; adding a mutation endpoint is outside owner 02's accepted boundary.
  QCOMPARE(http(info,"/v1/documents/"+id+"/source-patches",body).value("http_status").toInt(),404);QCOMPARE(editor->toPlainText(),original);
  const auto offset=original.toUtf8().indexOf("10");QVERIFY(offset>=0);
  SourceEditing::EditRequest external{id,editor->sourceRevision,editor->sourceFingerprint,{{offset,offset+2,"10","11"}}};
  QCOMPARE(SourceEditing::apply(*editor,external).error,SourceEditing::EditError::None);QCOMPARE(controller->state(),Controller::State::Idle);QVERIFY(!controller->commit());
  QCOMPARE(editor->toPlainText(),QString("cube(11);\n"));editor->undo();QCOMPARE(editor->toPlainText(),original);QVERIFY(!editor->canUndo());
}
