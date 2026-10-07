#include "TestInspectionDraft.h"
#include "gui/InspectionPresentationController.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/ScintillaEditor.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/parameter/ParameterWidget.h"
#include "gui/parameter/ParameterVirtualWidget.h"
#include "glview/InspectionMeshRenderer.h"
#include "glview/RenderSettings.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySetUtils.h"
#include "core/Tree.h"
#include "core/RenderVariables.h"
#include "core/Context.h"
#include <QSignalSpy>
#include <QTest>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QDockWidget>
#include <QMouseEvent>
#include <QClipboard>
#include <QApplication>
namespace {
std::shared_ptr<InspectionMeshSet> meshSet(MainWindow *window,const Selection::Stamp& stamp,
  std::string identity,uint64_t generation)
{
  auto scene=window->evaluationBinding->scene(stamp);
  if(!scene)throw std::runtime_error("Draft fixture needs captured live geometry");
  Tree tree(scene->root,scene->documentPath);GeometryEvaluator evaluator(tree);
  auto mesh=PolySetUtils::getGeometryAsPolySet(evaluator.evaluateGeometry(*scene->root,false));
  auto set=std::make_shared<InspectionMeshSet>();set->resultId=std::move(identity);set->geometryGeneration=generation;
  Transform3d placed=Transform3d::Identity();placed.translate(Vector3d(20,0,0));
  set->pieces={{1,{},mesh,Transform3d::Identity(),Color4f(1,0,0,1)},
               {2,{},mesh,placed,Color4f(0,1,0,1)}};return set;
}
}
void TestInspectionDraft::initTestCase()
{
  previousBackend=int(RenderSettings::inst()->backend3D);RenderSettings::inst()->backend3D=RenderBackend3D::ManifoldBackend;
  window->designActionAutoReload->setChecked(false);window->hide();window->show();window->raise();window->activateWindow();
  QVERIFY(QTest::qWaitForWindowExposed(window));QTRY_VERIFY(window->qglview->isValid());
}
void TestInspectionDraft::init()
{
  window->tabManager->actionNew();window->activeEditor->setPlainText("cube(10,center=true);\n");
  QSignalSpy done(window,&MainWindow::compilationDone);window->designActionRender->trigger();
  QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !GuiLocker::isLocked(),20000);QVERIFY(window->evaluationBinding->current());
  dynamic_cast<ScintillaEditor *>(window->activeEditor)->qsci->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);window->qglview->setFocus();QTest::qWait(20);
}
void TestInspectionDraft::cleanup()
{
  window->inspectionPresentation->cancelDraft();window->viewportTools->cancel(window->viewportTools->active(),Selection::CancelReason::Escape);
  window->activeEditor->parameterWidget->checkBoxAutoPreview->setChecked(false);
  window->activeEditor->parameterWidget->setModified(false);
  window->activeEditor->setContentModified(false);window->tabManager->closeCurrentTab();
}
void TestInspectionDraft::cleanupTestCase(){RenderSettings::inst()->backend3D=RenderBackend3D(previousBackend);}
void TestInspectionDraft::baseCandidateBasePreservesModelAndRejectsCapture()
{
  auto binding=window->evaluationBinding;auto base=binding->stamp();auto retained=binding->scene(base);
  auto editor=window->activeEditor;auto bytes=editor->toPlainText();auto revision=editor->sourceRevision;
  auto candidate=meshSet(window,base,"draft:1",1);auto presentation=window->inspectionPresentation;QObject lifetime;
  auto lease=presentation->beginDraft("Fake draft owner",&lifetime,base);QVERIFY(lease);QVERIFY(!presentation->beginDraft("Second",&lifetime,binding->stamp()));
  const auto camera=window->qglview->navigation()->current();QVERIFY(lease.setPending("Pending fake candidate"));QVERIFY(presentation->draftPending());
  QCOMPARE(window->findChild<QLabel *>("inspectionPreparationStatus")->text(),QString("Pending fake candidate"));
  QVERIFY(lease.showCandidate(1,candidate,"Draft candidate — not committed"));QVERIFY(binding->draftPixels());
  QVERIFY(binding->validateModel(base));QVERIFY(!binding->validate(base));QVERIFY(!binding->scene(binding->stamp()));
  QVERIFY(binding->modelDisplayCurrent(base));
  QVERIFY(!binding->displayCurrent(binding->stamp()));
  QVERIFY(!binding->displayScene(binding->stamp()));
  QCOMPARE(binding->stamp().resultId,base.resultId);QCOMPARE(editor->sourceRevision,revision);QCOMPARE(editor->toPlainText(),bytes);
  auto surface=window->qglview->renderer->prepareSurfacePick(window->qglview->edge_shader.get());QVERIFY(!surface.supported);
  QCOMPARE(window->selectionService->pick({10,10},binding->stamp()).error,Selection::PickError::Unsupported);
  bool replied=false;QString reason;window->annotationController->capture(base.resultId,[&](auto,auto,QString error){replied=true;reason=error;});
  QTRY_VERIFY(replied);QVERIFY(reason.contains("Draft"));
  bool canExport=true;QVERIFY(QMetaObject::invokeMethod(window,"canExport",Qt::DirectConnection,Q_RETURN_ARG(bool,canExport),Q_ARG(unsigned int,3)));QVERIFY(!canExport);
  window->fileActionExportImage->trigger();QVERIFY(!QApplication::activeModalWidget());
  QSignalSpy clipboardChanged(QApplication::clipboard(),&QClipboard::dataChanged);
  QVERIFY(QMetaObject::invokeMethod(window,"on_editActionCopyViewport_triggered",Qt::DirectConnection));QCOMPARE(clipboardChanged.size(),0);
  auto moved=camera;moved.translation+=Vector3d(1,2,0);window->qglview->navigation()->apply(moved);
  QVERIFY(!lease.showCandidate(1,candidate,"old serial"));QVERIFY(lease.showBase());QVERIFY(!binding->draftPixels());
  QVERIFY(binding->validateModel(base));lease.reset();QVERIFY(!presentation->draftActive());
  QVERIFY(binding->validate(binding->stamp()));QCOMPARE(binding->scene(binding->stamp()),retained);
  QVERIFY(window->qglview->navigation()->current()==moved);QCOMPARE(editor->sourceRevision,revision);
}
void TestInspectionDraft::masksAndEffectsRestoreWithoutCandidateInstallation()
{
  auto presentation=window->inspectionPresentation;auto binding=window->evaluationBinding;QObject lifetime;
  auto base=binding->stamp();auto live=meshSet(window,base,base.resultId.toStdString(),17);auto candidate=meshSet(window,base,"draft:2",2);
  int starts=0,effects=0;
  auto provider=presentation->registerMeshProvider("Fake retained provider",500,&lifetime,[&](auto,auto,auto done){++starts;done({InspectionPresentationController::Status::Ready,live,{}});return InspectionPresentationController::Cancel{};});
  auto effect=presentation->registerMeshEffect("Fake retained effect",50,&lifetime,[&](auto renderer){++effects;return renderer;});
  auto request=presentation->requestMeshes("Fake retained request",&lifetime);QTRY_COMPARE(presentation->status(),InspectionPresentationController::Status::Ready);
  QVERIFY(request.setMask({1,{}},binding->stamp(),17));auto rootRenderer=window->qglview->renderer;const auto count=effects;
  auto lease=presentation->beginDraft("Fake draft",&lifetime,binding->stamp());QVERIFY(lease);
  QVERIFY(lease.showCandidate(2,candidate,"Fake candidate"));auto pixels=window->qglview->renderer;
  effect.uniformsChanged();QCOMPARE(effects,count);QCOMPARE(window->qglview->renderer,pixels);QCOMPARE(presentation->meshes(),live);
  lease.reset();QVERIFY(effects>count);QCOMPARE(starts,1);QCOMPARE(presentation->meshes(),live);
  window->qglview->makeCurrent();auto restored=window->qglview->renderer->prepareSurfacePick(window->qglview->edge_shader.get());window->qglview->doneCurrent();QVERIFY(restored.supported);QVERIFY(restored.objects.empty());
  request.reset();effect.reset();provider.reset();
}
void TestInspectionDraft::lateProviderAndOwnerTeardown()
{
  auto binding=window->evaluationBinding;auto base=binding->stamp();auto presentation=window->inspectionPresentation;
  auto candidate=meshSet(window,base,"draft:3",3);auto live=meshSet(window,base,base.resultId.toStdString(),17);
  QObject providerOwner;InspectionPresentationController::Done late;int cancelled=0;
  auto provider=presentation->registerMeshProvider("Fake late provider",500,&providerOwner,[&](auto,auto,auto done){late=done;return InspectionPresentationController::Cancel([&]{++cancelled;});});
  auto request=presentation->requestMeshes("Fake late request",&providerOwner);QCOMPARE(presentation->status(),InspectionPresentationController::Status::Preparing);
  auto owner=std::make_unique<QObject>();auto lease=presentation->beginDraft("Fake draft",owner.get(),binding->stamp());QVERIFY(lease);QCOMPARE(cancelled,1);
  QVERIFY(lease.showCandidate(3,candidate,"Fake candidate"));auto pixels=window->qglview->renderer;
  auto deliver=late;deliver({InspectionPresentationController::Status::Ready,live,{}});QTest::qWait(20);QCOMPARE(window->qglview->renderer,pixels);
  QSignalSpy changed(presentation,&InspectionPresentationController::draftDisplayChanged);owner.reset();QVERIFY(!lease);QVERIFY(!presentation->draftActive());QCOMPARE(changed.size(),1);
  lease.reset();QCOMPARE(changed.size(),1);request.reset();provider.reset();deliver({InspectionPresentationController::Status::Ready,live,{}});QTest::qWait(20);QVERIFY(!binding->draftPixels());
}
void TestInspectionDraft::realSourceAndOrdinaryCompileCancelOnce()
{
  auto binding=window->evaluationBinding;auto presentation=window->inspectionPresentation;auto base=binding->stamp();QObject owner;
  auto candidate=meshSet(window,base,"draft:4",4);auto lease=presentation->beginDraft("Fake draft",&owner,base);QVERIFY(lease);QVERIFY(lease.showCandidate(4,candidate,"Fake candidate"));
  QSignalSpy changed(presentation,&InspectionPresentationController::draftDisplayChanged);window->activeEditor->setPlainText("cube(12);\n");
  QVERIFY(!presentation->draftActive());QVERIFY(!lease);QCOMPARE(changed.size(),1);QVERIFY(!binding->validateModel(base));
  QSignalSpy done(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY(!done.empty());QVERIFY(binding->current());
  base=binding->stamp();candidate=meshSet(window,base,"draft:5",5);lease=presentation->beginDraft("Fake draft",&owner,base);QVERIFY(lease);QVERIFY(lease.showCandidate(5,candidate,"Fake candidate"));
  done.clear();window->actionRenderPreview();QTRY_VERIFY(!done.empty());QVERIFY(!presentation->draftActive());QVERIFY(binding->current());QVERIFY(!binding->draftPixels());
}
void TestInspectionDraft::focusGroupAndSessionDrag()
{
  auto view=window->qglview;auto tools=window->viewportTools;QObject owner;int cancelled=0,moves=0,releases=0;
  auto dock=std::make_unique<QDockWidget>("Fake draft fields",window);auto *body=new QWidget;auto *layout=new QVBoxLayout(body);auto *field=new QLineEdit(body);layout->addWidget(field);dock->setWidget(body);window->addDockWidget(Qt::LeftDockWidgetArea,dock.get());dock->show();
  view->setFocus();QTest::qWait(20);auto token=tools->claimExternal("Fake draft session",&owner,[&](auto){++cancelled;});QVERIFY(token);
  QVERIFY(tools->setFocusGroup(token,{view,dock.get()}));field->setFocus();QTest::qWait(20);QCOMPARE(tools->active(),token);
  view->setFocus();QTest::qWait(20);Selection::DragCallbacks callbacks{[&](auto,auto){++moves;},[&](auto,auto){++releases;},[](auto){}};
  QVERIFY(tools->beginSessionDrag(token,{"Fake session drag",window->evaluationBinding->stamp(),true},{10,10},callbacks));
  QMouseEvent motion(QEvent::MouseMove,{20,20},{20,20},Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QVERIFY(tools->route(motion));
  QMouseEvent release(QEvent::MouseButtonRelease,{20,20},{20,20},Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QVERIFY(tools->route(release));
  QCOMPARE(moves,1);QCOMPARE(releases,1);QCOMPARE(tools->active(),token);QCOMPARE(cancelled,0);
  window->activeEditor->setFocus();QTRY_VERIFY(!tools->active());QCOMPARE(cancelled,1);
  tools->cancel(token,Selection::CancelReason::Escape);QCOMPARE(cancelled,1);
}
void TestInspectionDraft::replacementChecksTheOriginalEditor()
{
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());auto source=scene->inputs;
  auto candidate=source->withReplacement(source->root().fileKey,"cube(20);\n");QString detail;
  QVERIFY(Selection::EvaluationBinding::inputsCurrent(candidate,*window,&detail));
  window->activeEditor->setPlainText("cube(21);\n");QVERIFY(!Selection::EvaluationBinding::inputsCurrent(candidate,*window,&detail));
  QCOMPARE(QString::fromStdString(source->root().utf8),QString("cube(10,center=true);\n"));
}
void TestInspectionDraft::capturesAppliedCustomizerValues()
{
  const QString source="width=10; // [1:1:100]\ncube([width,2,3]);\n";
  window->activeEditor->setPlainText(source);QSignalSpy done(window,&MainWindow::compilationDone);
  window->actionRenderPreview();QTRY_VERIFY(!done.empty());QVERIFY(window->evaluationBinding->current());
  auto *parameters=window->activeEditor->parameterWidget;ParameterVirtualWidget *field=nullptr;
  for(auto *candidate:parameters->findChildren<ParameterVirtualWidget *>())if(candidate->getParameter()->name()=="width"){field=candidate;break;}
  QVERIFY(field);auto *number=dynamic_cast<NumberParameter *>(field->getParameter());QVERIFY(number);
  number->value=27;field->setValue();parameters->checkBoxAutoPreview->setChecked(true);done.clear();
  QVERIFY(QMetaObject::invokeMethod(field,"changed",Qt::DirectConnection,Q_ARG(bool,true)));QTRY_VERIFY(!done.empty());QVERIFY(window->evaluationBinding->current());
  const auto values=parameters->evaluationValues();QCOMPARE(values.at("width").get_value<double>(),27.0);
  QCOMPARE(window->activeEditor->toPlainText(),source);
  auto stamp=window->evaluationBinding->stamp();auto scene=window->evaluationBinding->scene(stamp);Tree tree(scene->root,scene->documentPath);GeometryEvaluator evaluator(tree);
  auto geometry=evaluator.evaluateGeometry(*scene->root,false);QVERIFY(geometry);QCOMPARE(geometry->getBoundingBox().max().x(),27.0);
  parameters->checkBoxAutoPreview->setChecked(false);parameters->setModified(false);
}

void TestInspectionDraft::ordinaryEvaluationUsesCapturedValuesOnce()
{
  const QString source="width=10; // [1:1:100]\ncube([$preview?99:10,width,3]);\n";
  auto *editor=window->activeEditor;editor->setPlainText(source);QSignalSpy done(window,&MainWindow::compilationDone);
  window->actionRenderPreview();QTRY_VERIFY(!done.empty());QVERIFY(window->evaluationBinding->current());
  auto *parameters=editor->parameterWidget;ParameterVirtualWidget *field=nullptr;
  for(auto *candidate:parameters->findChildren<ParameterVirtualWidget *>())if(candidate->getParameter()->name()=="width"){field=candidate;break;}
  QVERIFY(field);auto *number=dynamic_cast<NumberParameter *>(field->getParameter());QVERIFY(number);number->value=27;field->setValue();
  parameters->checkBoxAutoPreview->setChecked(true);done.clear();
  QVERIFY(QMetaObject::invokeMethod(field,"changed",Qt::DirectConnection,Q_ARG(bool,true)));QTRY_VERIFY(!done.empty());QVERIFY(window->evaluationBinding->current());
  const auto values=parameters->evaluationValues();const RenderVariables variables{true,0,window->qglview->cam};
  auto camera=window->qglview->navigation()->current();camera.translation+=Vector3d(3,4,0);window->qglview->navigation()->apply(camera);
  editor->setPlainText(source+"// Source change by fake consumer\n");done.clear();
  QVERIFY(window->evaluateCapturedVariables(variables,values));QTRY_VERIFY(!done.empty());QVERIFY(window->evaluationBinding->current());
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());Tree tree(scene->root,scene->documentPath);GeometryEvaluator evaluator(tree);
  auto geometry=evaluator.evaluateGeometry(*scene->root,false);QVERIFY(geometry);QCOMPARE(geometry->getBoundingBox().max().x(),99.0);QCOMPARE(geometry->getBoundingBox().max().y(),27.0);
  const auto capturedVpt=scene->backing->builtin->lookup_local_variable("$vpt");QVERIFY(capturedVpt);
  const auto& vpt=capturedVpt->toVector();const auto expected=variables.camera.getVpt();
  for(size_t axis=0;axis<3;++axis)QCOMPARE(vpt[axis].toDouble(),expected[axis]);
  // Later ordinary evaluation uses newly edited values and the current camera.
  // Customizer normally retains values across source edits, so defaults are not the baseline.
  parameters->checkBoxAutoPreview->setChecked(false);field=nullptr;
  for(auto *candidate:parameters->findChildren<ParameterVirtualWidget *>())if(candidate->getParameter()->name()=="width"){field=candidate;break;}
  QVERIFY(field);number=dynamic_cast<NumberParameter *>(field->getParameter());QVERIFY(number);number->value=42;field->setValue();
  QVERIFY(QMetaObject::invokeMethod(field,"changed",Qt::DirectConnection,Q_ARG(bool,false)));
  camera=window->qglview->navigation()->current();camera.translation+=Vector3d(7,8,0);window->qglview->navigation()->apply(camera);
  const auto ordinaryVpt=window->qglview->cam.getVpt();done.clear();window->actionRenderPreview();QTRY_VERIFY(!done.empty());QVERIFY(window->evaluationBinding->current());
  scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());Tree ordinary(scene->root,scene->documentPath);GeometryEvaluator normal(ordinary);
  geometry=normal.evaluateGeometry(*scene->root,false);QVERIFY(geometry);QCOMPARE(geometry->getBoundingBox().max().y(),42.0);
  const auto currentVpt=scene->backing->builtin->lookup_local_variable("$vpt");QVERIFY(currentVpt);
  for(size_t axis=0;axis<3;++axis)QCOMPARE(currentVpt->toVector()[axis].toDouble(),ordinaryVpt[axis]);
  parameters->checkBoxAutoPreview->setChecked(false);parameters->setModified(false);
}
