#include "TestClipping.h"
#include "gui/clipping/ClipMeshJob.h"
#include "gui/clipping/ClippingController.h"
#include "gui/clipping/ClippingDock.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/design/DesignDocument.h"
#include "core/design/DesignReferences.h"
#include "gui/InspectionPresentationController.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/ScintillaEditor.h"
#include "glview/ClippedMeshRenderer.h"
#include "glview/InspectionMeshRenderer.h"
#include "glview/SurfacePickMath.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySetUtils.h"
#include "geometry/GeometryCache.h"
#include "geometry/cgal/CGALCache.h"
#include "glview/RenderSettings.h"
#include "core/Tree.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLayout>
#include <QSplitter>
#include <QStyleOptionSpinBox>
#include <QToolButton>
#include <QLabel>
#include <QPushButton>
#include <QLineF>
#include "gui/QSettingsCached.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QWindow>
#include <algorithm>
#include <limits>

namespace {
using Status = InspectionPresentationController::Status;
bool compile(MainWindow& window, const QString& source, bool final = false) {
  if(window.activeEditor->toPlainText()!=source)window.activeEditor->setPlainText(source);
  QSignalSpy completed(&window, &MainWindow::compilationDone);
  if (final) window.designActionRender->trigger(); else window.actionRenderPreview();
  QElapsedTimer timer; timer.start();
  while ((completed.empty() || GuiLocker::isLocked()) && timer.elapsed()<20000) QTest::qWait(10);
  return !completed.empty() && window.evaluationBinding->current() && !GuiLocker::isLocked();
}
QPointF projected(QGLView& view, const Vector3d& world) {
  view.makeCurrent(); view.setupCamera();
  const Eigen::Map<const Eigen::Matrix4d> modelview(view.modelview), projection(view.projection);
  Eigen::Vector4d p = projection*modelview*Eigen::Vector4d(world.x(),world.y(),world.z(),1);
  p /= p.w(); view.doneCurrent();
  return {((p.x()+1)*view.cam.pixel_width/2)/view.devicePixelRatio(), ((1-p.y())*view.cam.pixel_height/2)/view.devicePixelRatio()};
}
struct Effect {
  MainWindow& window;
  std::shared_ptr<ClipState> state=std::make_shared<ClipState>();
  std::shared_ptr<InspectionMeshRenderer> base;
  InspectionPresentationController::Token effect;
  explicit Effect(MainWindow& ownerWindow) : window(ownerWindow) {
    effect = window.inspectionPresentation->registerMeshEffect("clipping-test",20,&window,[this](std::shared_ptr<Renderer> child) {
      base = std::dynamic_pointer_cast<InspectionMeshRenderer>(child);
      auto renderer = std::make_shared<ClippedMeshRenderer>(child,state);
      this->window.qglview->makeCurrent();
      try { renderer->prepare(this->window.qglview->edge_shader.get()); }
      catch (...) { renderer.reset(); this->window.qglview->doneCurrent(); throw; }
      this->window.qglview->doneCurrent(); return renderer;
    });
  }
  ~Effect(){effect.reset();this->window.qglview->makeCurrent();base.reset();this->window.qglview->doneCurrent();}
  void planes(std::vector<ClipPlane> planes) {
    *state = prepareClipState(std::move(planes),this->window.qglview->renderer->getBoundingBox(),state->generation+1);
    effect.uniformsChanged();
  }
};

}
void TestClipping::initTestCase() {
  window->designActionAutoReload->setChecked(false);
  window->hide(); window->show(); window->raise(); window->activateWindow();
  QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->qglview->isValid());
  window->tabManager->actionNew();
  window->qglview->makeCurrent(); qInfo("Clipping renderer: %s",window->qglview->getRendererInfo().c_str()); window->qglview->doneCurrent();
}
void TestClipping::rearSurfacesAndFrozenPreview() {
  struct Fixture { const char *source; Vector3d point,normal,planeNormal; };
  const std::vector<Fixture> fixtures={
    {"cube(10,center=true);",{0,5,0},{0,1,0},{0,1,0}},
    {"difference(){cube(10,center=true);cube(8,center=true);}",{0,4,0},{0,-1,0},{0,1,0}},
    {"rotate([0,0,30]) difference(){cylinder(h=10,r=5,center=true,$fn=96);cylinder(h=12,r=4,center=true,$fn=96);}",{0,4,0},{0,-1,0},{0.2,1,0.1}},
    {"intersection(){cube(10,center=true);translate([0,2,0])cube(10,center=true);}",{0,5,0},{0,1,0},{0,1,0}},
    {"cube($preview?10:100,center=true);",{0,5,0},{0,1,0},{0,1,0}}
  };
  for (size_t i=0;i<fixtures.size();++i) for(bool final:{false,true}) {
    if(final && i==4)continue;
    const auto& fixture=fixtures[i]; QVERIFY(compile(*window,fixture.source,final));
    auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());
    Tree frozen(scene->root,scene->documentPath); GeometryEvaluator evaluator(frozen);
    auto reference=evaluator.evaluateGeometry(*scene->root,false);
    ClipMeshJob jobs(window); auto presentation=window->inspectionPresentation;
    auto provider=presentation->registerMeshProvider("clipping-root-test",0,&jobs,[&jobs](auto scene,auto stamp,auto done){return jobs.start(scene,stamp,done);});
    Effect effect(*window); auto request=presentation->requestMeshes("clipping-test",&jobs);
    QTRY_VERIFY_WITH_TIMEOUT(presentation->status()==Status::Ready,20000);
    QVERIFY2(presentation->status()==Status::Ready,qPrintable(presentation->reason()));
    QVERIFY(presentation->meshes());
    QVERIFY(window->qglview->renderer->getBoundingBox().min().isApprox(reference->getBoundingBox().min()));
    QVERIFY(window->qglview->renderer->getBoundingBox().max().isApprox(reference->getBoundingBox().max()));
    if(final) { QCOMPARE(jobs.metrics().evaluations.load(),uint64_t(0)); QCOMPARE(jobs.metrics().reusedFinal.load(),uint64_t(1)); }
    effect.planes({{{0,0,0},fixture.planeNormal}});
    auto view=window->qglview;
    view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),-fixture.point,40,45,Camera::ProjectionType::ORTHOGONAL});
    auto pixel=projected(*view,fixture.point); auto image=view->grabFrame(QGLView::CaptureContent::ModelOnly);
    const auto physical=SurfacePickMath::pixel(pixel.x(),pixel.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height); QVERIFY(physical);
    QVERIFY(image.pixelColor(physical->x(),view->cam.pixel_height-1-physical->y())!=image.pixelColor(0,0));
    auto picked=window->selectionService->pick(pixel,window->evaluationBinding->stamp());
    QVERIFY2(picked.hit.has_value(),qPrintable(picked.detail));
    qInfo("Rear fixture %zu final %d: point %.8g %.8g %.8g, normal %.8g %.8g %.8g",i,final,picked.hit->worldPoint.x(),picked.hit->worldPoint.y(),picked.hit->worldPoint.z(),picked.hit->outwardNormal.x(),picked.hit->outwardNormal.y(),picked.hit->outwardNormal.z());
    QVERIFY(std::abs(picked.hit->worldPoint.y()-fixture.point.y())<.03);
    QVERIFY(picked.hit->outwardNormal.dot(fixture.normal)>.99);
    request.reset();effect.effect.reset();provider.reset();
  }
}
void TestClipping::suppliedPiecesAndOwnership() {
  QVERIFY(compile(*window,"cube(10,center=true);"));
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp()); Tree tree(scene->root,scene->documentPath); GeometryEvaluator evaluator(tree);
  auto mesh=PolySetUtils::getGeometryAsPolySet(evaluator.evaluateGeometry(*scene->root,false));
  QObject owner; auto presentation=window->inspectionPresentation; ClipMeshJob root(window);
  auto low=presentation->registerMeshProvider("clipping-root-test",0,&root,[&](auto scene,auto stamp,auto done){return root.start(scene,stamp,done);});
  auto supplied=std::make_shared<InspectionMeshSet>(); supplied->resultId=window->evaluationBinding->stamp().resultId.toStdString(); supplied->geometryGeneration=22;
  Transform3d mirror=Transform3d::Identity();mirror.scale(Vector3d(-1,1,1));Transform3d rear=mirror;rear.translate(Vector3d(0,14,0));
  supplied->pieces={{uint64_t(1)<<40,{},mesh,mirror,Color4f(1.f,0.f,0.f,1.f)},{77,{},mesh,rear,Color4f(0.f,1.f,0.f,1.f)}};
  auto high=presentation->registerMeshProvider("piece-fixture",200,&owner,[supplied](auto,auto,auto done){done({Status::Ready,supplied,{}});return InspectionPresentationController::Cancel{};});
  Effect effect(*window);auto request=presentation->requestMeshes("piece-owner",&owner);
  QTRY_VERIFY(presentation->status()==Status::Ready);
  QVERIFY(request.setMask({1,{77}},presentation->stamp(),22));
  effect.planes({{{0,14,0},{0,1,0}}});auto view=window->qglview;
  view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d(0,-19,0),50,45,Camera::ProjectionType::ORTHOGONAL});
  auto picked=window->selectionService->pick(projected(*view,{0,19,0}),window->evaluationBinding->stamp());QVERIFY(picked.hit);QCOMPARE(picked.hit->object.drawId,uint64_t(77));QVERIFY(std::abs(picked.hit->worldPoint.y()-19)<1e-4);QVERIFY(picked.hit->outwardNormal.dot(Vector3d::UnitY())>.999);
  effect.effect.reset();QVERIFY(presentation->meshes()==supplied);
  picked=window->selectionService->pick(projected(*view,{0,9,0}),window->evaluationBinding->stamp());QVERIFY(picked.hit);QCOMPARE(picked.hit->object.drawId,uint64_t(77));QVERIFY(std::abs(picked.hit->worldPoint.y()-9)<1e-4);
  high.reset();auto failed=presentation->registerMeshProvider("failed-pieces",200,&owner,[](auto,auto,auto done){done({Status::Failed,{},"fixture failure"});return InspectionPresentationController::Cancel{};});
  QTRY_VERIFY(presentation->status()==Status::Failed);QCOMPARE(root.metrics().jobs.load(),uint64_t(0));
  failed.reset(); auto pending=presentation->registerMeshProvider("pending-pieces",200,&owner,[](auto,auto,auto){return InspectionPresentationController::Cancel{};});
  QTRY_VERIFY(presentation->status()==Status::Preparing);presentation->cancelPreparation();QCOMPARE(presentation->status(),Status::Cancelled);QCOMPARE(root.metrics().jobs.load(),uint64_t(0));
  request.reset();pending.reset();low.reset();
}
void TestClipping::uniformUpdatesAndPerformance() {
  QVERIFY(compile(*window,"sphere(r=10,$fn=320);"));
  ClipMeshJob jobs(window);auto presentation=window->inspectionPresentation;
  auto provider=presentation->registerMeshProvider("clipping-root-test",0,&jobs,[&](auto scene,auto stamp,auto done){return jobs.start(scene,stamp,done);});
  Effect effect(*window);auto request=presentation->requestMeshes("performance",&jobs);
  QTRY_VERIFY_WITH_TIMEOUT(presentation->status()!=Status::Preparing,20000);QVERIFY2(presentation->status()==Status::Ready,qPrintable(presentation->reason()));QVERIFY(effect.base);
  auto view=window->qglview;view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d::Zero(),60,45,Camera::ProjectionType::ORTHOGONAL});
  auto oldMin=view->minimumSize(),oldMax=view->maximumSize();view->setFixedSize(qRound(1280/view->devicePixelRatio()),qRound(720/view->devicePixelRatio()));QTest::qWait(50);
  QCOMPARE(view->cam.pixel_width,1280);QCOMPARE(view->cam.pixel_height,720);
  const auto uploads=effect.base->uploadCount(),work=jobs.metrics().jobs.load(),evaluations=jobs.metrics().evaluations.load();QSignalSpy sourceEvaluations(window,&MainWindow::compilationDone);
  const auto triangles=view->renderer->triangleStatistics().count;QVERIFY(triangles && *triangles>=100000);
  QJsonArray timings;
  for(int planeCount:{1,8}) {
    std::vector<double> milliseconds;
    for(int frame=0;frame<110;++frame) {
      std::vector<ClipPlane> planes;for(int i=0;i<planeCount;++i)planes.push_back({{0,0,.01*frame},{0,1,.01*i}});
      QElapsedTimer clock;clock.start();effect.planes(std::move(planes));view->makeCurrent();glBindFramebuffer(GL_FRAMEBUFFER,view->defaultFramebufferObject());
      view->GLView::paintGL();glFinish();double ms=clock.nsecsElapsed()/1e6;view->doneCurrent();if(frame>=10)milliseconds.push_back(ms);
    }
    std::sort(milliseconds.begin(),milliseconds.end());qInfo("100k mesh %d planes: p50 %.3f ms p95 %.3f ms; preparation %.3f ms",planeCount,milliseconds[50],milliseconds[95],jobs.metrics().preparationMilliseconds);
    timings.append(QJsonObject{{"planes",planeCount},{"p50_ms",milliseconds[50]},{"p95_ms",milliseconds[95]}});
    QVERIFY2(milliseconds[95]<=33,"Clipping frame target exceeded; investigate measured GPU timing");
  }
  QCOMPARE(effect.base->uploadCount(),uploads);QCOMPARE(jobs.metrics().jobs.load(),work);QCOMPARE(jobs.metrics().evaluations.load(),evaluations);QVERIFY(sourceEvaluations.empty());
  const auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");if(!output.isEmpty()){QFile file(output+"/clipping-performance.json");QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(QJsonObject{{"triangles",qint64(*triangles)},{"width",1280},{"height",720},{"preparation_ms",jobs.metrics().preparationMilliseconds},{"jobs_after_updates",qint64(work)},{"uploads_after_updates",qint64(uploads)},{"timings",timings}}).toJson());}
  view->setMinimumSize(oldMin);view->setMaximumSize(oldMax);request.reset();effect.effect.reset();provider.reset();
}
void TestClipping::init() {
  auto controller=Clipping::Controller::forWindow(*window);controller->setGlobalEnabled(false);window->tabManager->actionNew();controller->setGlobalEnabled(false);controller->dock->hide();
}
void TestClipping::numericalGpuBoundaries() {
  QVERIFY(compile(*window,"cube(1);"));auto presentation=window->inspectionPresentation;
  for(double scale:{1e-4,1.,1e4})for(double translation:{0.,1e5})for(bool mirrored:{false,true}) {
    Vector3d center=scale*Vector3d(translation,-translation,translation),normal=Vector3d(1,2,3).normalized();
    Eigen::Quaterniond rotation=Eigen::Quaterniond::FromTwoVectors(Vector3d::UnitZ(),normal);
    Transform3d transform=Transform3d::Identity();transform.translate(center);transform.rotate(rotation);transform.scale(Vector3d(mirrored?-scale:scale,scale,scale));
    const double separation=std::sqrt(8.)*1e-4;
    auto sheet=[](double height){auto mesh=std::make_shared<PolySet>(3);mesh->vertices={{-1,-1,height},{1,-1,height},{1,1,height},{-1,1,height}};mesh->indices={{0,1,2},{0,2,3}};mesh->setTriangular(true);return mesh;};
    auto set=std::make_shared<InspectionMeshSet>();set->resultId=presentation->stamp().resultId.toStdString();set->geometryGeneration=1;
    set->pieces={{101,{},sheet(0),transform,Color4f(1.f,0.f,0.f,1.f)},{202,{},sheet(separation),transform,Color4f(0.f,1.f,0.f,1.f)}};
    QObject owner;auto provider=presentation->registerMeshProvider("numerical-sheets",200,&owner,[set](auto,auto,auto done){done({Status::Ready,set,{}});return InspectionPresentationController::Cancel{};});
    Effect effect(*window);auto request=presentation->requestMeshes("numerics",&owner);QTRY_VERIFY(presentation->status()==Status::Ready);
    auto view=window->qglview;const Vector3d point=center+normal*separation*scale;
    view->navigation()->apply({CameraPose::upright(-normal),-point,3.5*scale,45,Camera::ProjectionType::ORTHOGONAL});
    const auto pixel=projected(*view,point);
    auto hit=window->selectionService->pick(pixel,presentation->stamp());QVERIFY(hit.hit);QCOMPARE(hit.hit->object.drawId,uint64_t(101));
    for(int count:{1,8}) {
      effect.planes(std::vector<ClipPlane>(count,ClipPlane{center,normal}));
      hit=window->selectionService->pick(pixel,presentation->stamp());QVERIFY2(hit.hit.has_value(),qPrintable(hit.detail));QCOMPARE(hit.hit->object.drawId,uint64_t(202));
      const double diagonal=view->renderer->getBoundingBox().sizes().norm();
      QVERIFY(std::abs((hit.hit->worldPoint-point).dot(normal))<1e-5*diagonal);QVERIFY(std::abs(hit.hit->outwardNormal.dot(normal))>.999);
      auto image=view->grabFrame(QGLView::CaptureContent::ModelOnly);auto physical=SurfacePickMath::pixel(pixel.x(),pixel.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height);QVERIFY(physical);
      auto color=image.pixelColor(physical->x(),view->cam.pixel_height-1-physical->y());qInfo("Retained pixel RGBA %d %d %d %d",color.red(),color.green(),color.blue(),color.alpha());QVERIFY(color.green()>color.red());
      qInfo("GPU scale %.8g translation %.8g mirrored %d planes %d epsilon %.10g separate wall %.10g depth error %.10g",scale,translation,mirrored,count,effect.state->planes.front().epsilon,separation*scale,std::abs((hit.hit->worldPoint-point).dot(normal)));
    }
    effect.planes({});hit=window->selectionService->pick(pixel,presentation->stamp());QVERIFY(hit.hit);QCOMPARE(hit.hit->object.drawId,uint64_t(101));
    request.reset();effect.effect.reset();provider.reset();
  }
}
void TestClipping::backendNumericalBoundaries() {
  struct BackendRestore { RenderBackend3D previous=RenderSettings::inst()->backend3D;~BackendRestore(){RenderSettings::inst()->backend3D=previous;} } restore;
  QJsonArray evidence;auto presentation=window->inspectionPresentation;
  for(int mode=0;mode<3;++mode) {
    RenderSettings::inst()->backend3D=mode==2?RenderBackend3D::CGALBackend:RenderBackend3D::ManifoldBackend;
    GeometryCache::instance()->clear();CGALCache::instance()->clear();
    for(double scale:{1e-4,1.,1e4})for(double translation:{0.,1e5})for(bool mirrored:{false,true}) {
      Vector3d center=scale*Vector3d(translation,-translation,translation),normal=Vector3d(1,2,3).normalized();
      Transform3d transform=Transform3d::Identity();transform.translate(center);transform.rotate(Eigen::Quaterniond::FromTwoVectors(Vector3d::UnitZ(),normal));transform.scale(Vector3d(mirrored?-scale:scale,scale,scale));
      double gap=.0003;
      for(int iteration=0;iteration<8;++iteration){BoundingBox bounds;for(int corner=0;corner<8;++corner)bounds.extend(transform*Vector3d(corner&1?1:-1,corner&2?1:-1,corner&4?gap+.2:-.2));gap=1e-4*bounds.sizes().norm()/scale;}
      QString matrix="[";for(int row=0;row<4;++row){if(row)matrix+=",";matrix+="[";for(int column=0;column<4;++column){if(column)matrix+=",";matrix+=QString::number(transform.matrix()(row,column),'g',17);}matrix+="]";}matrix+="]";
      const QString source="multmatrix("+matrix+")union(){translate([-1,-1,-.2])cube([2,2,.2]);translate([-1,-1,"+QString::number(gap,'g',17)+"])cube([2,2,.2]);}";
      QVERIFY(compile(*window,source,mode!=0));QString geometryType=mode?QString::fromUtf8(typeid(*window->rootGeom).name()):"Frozen preview tree";
      if(mode==1)QVERIFY2(geometryType.contains("Manifold"),qPrintable(geometryType));if(mode==2)QVERIFY2(geometryType.contains("CGAL"),qPrintable(geometryType));
      ClipMeshJob jobs(window);auto provider=presentation->registerMeshProvider("backend-numerics",0,&jobs,[&](auto scene,auto stamp,auto done){return jobs.start(scene,stamp,done);});
      Effect effect(*window);auto request=presentation->requestMeshes("backend-numerics",&jobs);QTRY_VERIFY_WITH_TIMEOUT(presentation->status()!=Status::Preparing,20000);QVERIFY2(presentation->status()==Status::Ready,qPrintable(presentation->reason()));
      QCOMPARE(jobs.metrics().evaluations.load(),uint64_t(mode?0:1));QCOMPARE(jobs.metrics().reusedFinal.load(),uint64_t(mode?1:0));
      auto view=window->qglview;const Vector3d point=center+normal*gap*scale;view->navigation()->apply({CameraPose::upright(-normal),-point,3.5*scale,45,Camera::ProjectionType::ORTHOGONAL});const auto pixel=projected(*view,point);
      const double worldDiagonal=view->renderer->getBoundingBox().sizes().norm();auto hit=window->selectionService->pick(pixel,presentation->stamp());QVERIFY(hit.hit);qInfo("Backend %d scale %.8g translation %.8g mirrored %d unclipped signed depth %.10g expected %.10g diagonal %.10g",mode,scale,translation,mirrored,(hit.hit->worldPoint-center).dot(normal),-.2*scale,worldDiagonal);QVERIFY(std::abs((hit.hit->worldPoint-(center-normal*.2*scale)).dot(normal))<1e-5*worldDiagonal);
      for(int count:{1,8}) {
        effect.planes(std::vector<ClipPlane>(count,ClipPlane{center,normal}));hit=window->selectionService->pick(pixel,presentation->stamp());QVERIFY2(hit.hit.has_value(),qPrintable(hit.detail));
        double diagonal=view->renderer->getBoundingBox().sizes().norm(),error=std::abs((hit.hit->worldPoint-point).dot(normal));QVERIFY(std::abs(gap*scale/diagonal-1e-4)<1e-9);QVERIFY2(error<1e-5*diagonal,qPrintable(QString::number(error,'g',17)));QVERIFY(hit.hit->outwardNormal.dot(-normal)>.999);
        auto image=view->grabFrame(QGLView::CaptureContent::ModelOnly);auto physical=SurfacePickMath::pixel(pixel.x(),pixel.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height);QVERIFY(physical);QVERIFY(image.pixelColor(physical->x(),view->cam.pixel_height-1-physical->y())!=image.pixelColor(0,0));
        evidence.append(QJsonObject{{"mode",mode==0?"preview-derived":mode==1?"Manifold":"CGAL"},{"geometryType",geometryType},{"scale",scale},{"translation_scale_units",translation},{"mirrored",mirrored},{"planes",count},{"diagonal",diagonal},{"epsilon",effect.state->planes.front().epsilon},{"wall_separation",gap*scale},{"depth_error",error},{"normal_dot",hit.hit->outwardNormal.dot(-normal)}});
      }
      effect.planes({});hit=window->selectionService->pick(pixel,presentation->stamp());QVERIFY(hit.hit);QVERIFY(std::abs((hit.hit->worldPoint-(center-normal*.2*scale)).dot(normal))<1e-5*worldDiagonal);
      request.reset();effect.effect.reset();provider.reset();
    }
  }
  QCOMPARE(evidence.size(),72);qInfo("Native backend boundary matrix: 72 samples across frozen preview, Manifold and CGAL");
  const auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");if(!output.isEmpty()){QFile file(output+"/clipping-backend-numerics.json");QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(evidence).toJson());}
}
void TestClipping::nativePieceRoles() {
  QVERIFY(compile(*window,"cube(10,center=true);color([0,1,0])translate([40,0,0])%cube(4,center=true);translate([-15,0,0])#cube(4,center=true);"));
  auto presentation=window->inspectionPresentation;ClipMeshJob jobs(window);auto provider=presentation->registerMeshProvider("native-roles",0,&jobs,[&](auto scene,auto stamp,auto done){return jobs.start(scene,stamp,done);});Effect effect(*window);auto request=presentation->requestMeshes("native-roles",&jobs);QTRY_VERIFY_WITH_TIMEOUT(presentation->status()!=Status::Preparing,20000);QVERIFY2(presentation->status()==Status::Ready,qPrintable(presentation->reason()));
  bool background=false,highlight=false;for(const auto& piece:presentation->meshes()->pieces){auto bounds=piece.localToWorld*piece.mesh->getBoundingBox();if(piece.role==InspectionPiece::Role::Background){background=true;QVERIFY(bounds.center().isApprox(Vector3d(40,0,0)));}if(piece.role==InspectionPiece::Role::Highlight){highlight=true;QVERIFY(bounds.center().isApprox(Vector3d(-15,0,0)));}}QVERIFY(background);QVERIFY(highlight);
  auto view=window->qglview;view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d(-40,0,0),30,45,Camera::ProjectionType::ORTHOGONAL});effect.planes({{{0,0,0},{0,1,0}}});auto pixel=projected(*view,{40,2,0});auto hit=window->selectionService->pick(pixel,presentation->stamp());QVERIFY(!hit.hit);auto image=view->grabFrame(QGLView::CaptureContent::ModelOnly);auto physical=SurfacePickMath::pixel(pixel.x(),pixel.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height);QVERIFY(physical);QVERIFY(image.pixelColor(physical->x(),view->cam.pixel_height-1-physical->y())!=image.pixelColor(0,0));
  request.reset();effect.effect.reset();provider.reset();
}
void TestClipping::cancellationAndUnsupportedInputs() {
  QVERIFY(compile(*window,"cube(10);"));auto presentation=window->inspectionPresentation;ClipMeshJob jobs(window);
  auto provider=presentation->registerMeshProvider("clipping-root-test",0,&jobs,[&](auto scene,auto stamp,auto done){return jobs.start(scene,stamp,done);});
  auto original=window->qglview->renderer;
  InspectionPresentationController::Token request;
  { GuiLocker anotherGeometryTask;request=presentation->requestMeshes("cancel-root",&jobs);QTest::qWait(30);QCOMPARE(presentation->status(),Status::Preparing);QCOMPARE(jobs.metrics().jobs.load(),uint64_t(0));presentation->cancelPreparation(); }
  QTest::qWait(30);QCOMPARE(presentation->status(),Status::Cancelled);QCOMPARE(jobs.metrics().jobs.load(),uint64_t(0));QVERIFY(window->qglview->renderer==original);request.reset();
  window->viewModeThrownTogether();original=window->qglview->renderer;request=presentation->requestMeshes("unsupported-thrown-together",&jobs);QTRY_VERIFY(presentation->status()!=Status::Preparing);QCOMPARE(presentation->status(),Status::Unsupported);QVERIFY(presentation->reason().contains("Thrown Together"));QVERIFY(window->qglview->renderer==original);request.reset();window->viewModePreview();
  QVERIFY(compile(*window,"square(10);"));request=presentation->requestMeshes("unsupported-2d",&jobs);QTRY_VERIFY_WITH_TIMEOUT(presentation->status()!=Status::Preparing,20000);QCOMPARE(presentation->status(),Status::Unsupported);QVERIFY(!presentation->reason().isEmpty());request.reset();
  QVERIFY(compile(*window,"union(){}"));request=presentation->requestMeshes("empty-root",&jobs);QTRY_VERIFY_WITH_TIMEOUT(presentation->status()!=Status::Preparing,20000);QCOMPARE(presentation->status(),Status::Empty);QVERIFY(presentation->meshes()->pieces.empty());request.reset();provider.reset();
}
void TestClipping::canonicalDockLifecycle() {
  const QString source="height=40;\n@{type:Section}\nmiddle={id:\"middle\",label:\"Middle\",origin:[0,0,height/2],normal:[0,0,1],up:[0,1,0],adjust:{offset:0}};\n@{type:View}\nlinked={id:\"linked\",direction:[0,1,0],up:[0,0,1],frame:{domain:\"design\",id:\"middle\"}};\ncube([20,20,height]);\n";
  QVERIFY(compile(*window,source));auto controller=Clipping::Controller::forWindow(*window);controller->dock->show();controller->select("middle");
  QVERIFY(controller->enable("middle",true));controller->setGlobalEnabled(true);QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);
  QCOMPARE(controller->clippingState()->planes.size(),size_t(1));QVERIFY(controller->frame("middle")->translation().isApprox(Vector3d(0,0,20)));
  QVERIFY(!controller->dock->findChild<QWidget *>("sectionOffset"));
  QVERIFY(!controller->dock->findChild<QWidget *>("sectionOffsetSlider"));
  QVERIFY(!controller->dock->findChild<QWidget *>("sectionHandles"));
  QVERIFY(!controller->dock->findChild<QWidget *>("sectionAngleX"));
  QVERIFY(controller->dock->findChild<QToolButton *>("sectionHelp"));
  auto originX=controller->dock->findChild<QDoubleSpinBox *>("sectionOriginX");QVERIFY(originX && originX->isEnabled());originX->setValue(3);QVERIFY(QMetaObject::invokeMethod(originX,"editingFinished"));QVERIFY(window->activeEditor->toPlainText().contains("height/2"));QVERIFY(compile(*window,window->activeEditor->toPlainText()));QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);QVERIFY(controller->frame("middle")->translation().isApprox(Vector3d(3,0,20)));window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);QVERIFY(compile(*window,source));QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);
  controller->setHelperEnabled("middle",true);window->qglview->navigation()->apply({CameraPose::upright(Vector3d(-1,-1,.7)),Vector3d(-10,-10,-20),110,45,Camera::ProjectionType::ORTHOGONAL});QTest::qWait(50);const auto screenshot=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");if(!screenshot.isEmpty())QVERIFY(window->grab().save(screenshot+"/clipping-dock.png"));
  window->qglview->repaint();const auto planeImage=window->qglview->grabFramebuffer();
  const auto planePixel=(projected(*window->qglview,Vector3d(-5,-5,20))*window->qglview->devicePixelRatio()).toPoint();
  QVERIFY(planeImage.rect().contains(planePixel));const auto planeColor=planeImage.pixelColor(planePixel);
  QCOMPARE(planeColor.alpha(),255);QVERIFY(planeColor.green()>planeColor.red());QVERIFY(planeColor.blue()>planeColor.red());
  QVERIFY(controller->beginGesture("middle"));Design::FieldChange change{Design::ChangeKind::Offset};change.offset=-35;QVERIFY(controller->preview({change}));QVERIFY(window->activeEditor->toPlainText()==source);QVERIFY(controller->frame("middle")->translation().isApprox(Vector3d(0,0,-15)));QVERIFY(controller->finishGesture());
  QVERIFY(window->activeEditor->toPlainText().contains("height/2"));QVERIFY(window->activeEditor->toPlainText().contains("offset:-35"));window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);QVERIFY(compile(*window,window->activeEditor->toPlainText()));QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);
  window->activeEditor->redo();QVERIFY(window->activeEditor->toPlainText().contains("offset:-35"));QVERIFY(compile(*window,window->activeEditor->toPlainText()));QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);
  controller->setGlobalEnabled(false);QVERIFY(controller->remove("middle"));QVERIFY(compile(*window,window->activeEditor->toPlainText()));auto all=controller->records();auto context=Design::ResolveContext{"","","",controller->bounds(),all,{}};auto resolved=Design::resolve({"design","middle"},context);QCOMPARE(resolved.state,Design::ResolutionState::Missing);
  window->activeEditor->undo();QVERIFY(compile(*window,window->activeEditor->toPlainText()));QVERIFY(controller->records().count("middle"));QVERIFY(controller->records().count("linked"));
  auto id=controller->createSection();QVERIFY(id);QVERIFY(controller->edit(*id,{{Design::ChangeKind::Label,"Named plane"}}));auto before=window->activeEditor->toPlainText();QVERIFY(controller->promote(*id));QVERIFY(window->activeEditor->toPlainText()!=before);
  // Check the interval before evaluation, not just the final evaluated source record.
  QVERIFY(!controller->evaluationCurrent());QVERIFY(controller->records().at(*id)->source.has_value());
  QCOMPARE(controller->selectedId(),*id);
  auto rows=controller->dock->findChild<QTreeWidget *>("sectionRows");QVERIFY(rows);QVERIFY(rows->currentItem());
  QCOMPARE(rows->currentItem()->text(0),QString("Named plane"));
  QCOMPARE(rows->currentItem()->text(1),QString("Source"));
  QVERIFY(controller->editable(*id));
  window->editorDock->hide();QVERIFY(!window->activeEditor->isSourceReadOnly());
  const auto inserted=window->activeEditor->toPlainText();
  QVERIFY(controller->setOrigin(*id,Vector3d(0,0,3)));QCOMPARE(controller->frame(*id)->translation().z(),3.);
  QVERIFY(controller->setOrigin(*id,Vector3d(0,0,14)));QCOMPARE(controller->frame(*id)->translation().z(),14.);
  QVERIFY(controller->edit(*id,{{Design::ChangeKind::Label,"Renamed source plane"}}));
  QVERIFY(window->activeEditor->toPlainText().contains("Renamed source plane"));
  window->activeEditor->undo();window->activeEditor->undo();window->activeEditor->undo();
  QCOMPARE(window->activeEditor->toPlainText(),inserted);QCOMPARE(controller->frame(*id)->translation().z(),0.);
  window->activeEditor->undo();
  QVERIFY(controller->records().count(*id));QVERIFY(!controller->records().at(*id)->source);
  window->activeEditor->redo();QVERIFY(controller->records().at(*id)->source.has_value());
  QVERIFY(compile(*window,window->activeEditor->toPlainText()));QVERIFY(controller->records().at(*id)->source);
  QTemporaryDir directory;QVERIFY(directory.isValid());auto path=directory.filePath("source-sections.scad");QVERIFY(window->tabManager->saveAs(window->activeEditor,path));QVERIFY(compile(*window,window->activeEditor->toPlainText()));
  QVERIFY(controller->enable(*id,true));controller->setHelperEnabled(*id,true);auto stableId=*id;window->tabManager->closeCurrentTab();window->tabManager->open(path);QVERIFY(compile(*window,window->activeEditor->toPlainText()));QVERIFY(controller->records().count(stableId));QVERIFY(controller->enabled(stableId));QVERIFY(controller->helperEnabled(stableId));QCOMPARE(controller->records().at(stableId)->label,std::string("Named plane"));
  QVERIFY(!QFile::exists(path+".sections.json"));controller->setGlobalEnabled(false);
}
void TestClipping::deleteAndSessionPersistence() {
  QVERIFY(compile(*window,"cube(20,center=true);"));
  auto controller=Clipping::Controller::forWindow(*window);controller->dock->show();
  auto id=controller->createSection();QVERIFY(id);controller->setGlobalEnabled(false);
  auto editor=window->activeEditor;
  const auto identity=editor->filepath.isEmpty()?editor->documentId():QFileInfo(editor->filepath).absoluteFilePath();
  const auto key="inspection/sections/"+QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(),QCryptographicHash::Sha256).toHex())+"/";
  QSettingsCached settings;
  QVERIFY(!settings.value(key+"enabledIds").toStringList().contains(QString::fromStdString(*id)));
  auto remove=controller->dock->findChild<QPushButton *>("sectionDelete");QVERIFY(remove && remove->isEnabled());
  remove->click();
  QVERIFY(!controller->records().count(*id));QVERIFY(!controller->enabled(*id));QVERIFY(!controller->helperEnabled(*id));
  window->activeEditor->undo();QVERIFY(controller->records().count(*id));
  // Reproduce the orphan left by older builds: a remembered ID without its definition.
  QVERIFY(controller->enable(*id,true));controller->setGlobalEnabled(false);
  auto document=Design::DesignDocument::forEditor(*editor,*window);
  QVERIFY(document->remove(*id,window->evaluationBinding->stamp()));
  controller->select(*id);QVERIFY(remove->isEnabled());remove->click();
  QVERIFY(!controller->enabled(*id));QVERIFY(!controller->helperEnabled(*id));
  auto rows=controller->dock->findChild<QTreeWidget *>("sectionRows");
  for(int i=0;i<rows->topLevelItemCount();++i)QVERIFY(rows->topLevelItem(i)->data(0,Qt::UserRole).toString()!=QString::fromStdString(*id));
}

void TestClipping::presetsLimitsAndSourceOnly() {
  QVERIFY(compile(*window,"cube(20,center=true);"));auto controller=Clipping::Controller::forWindow(*window);
  std::vector<std::string> ids;for(int i=0;i<9;++i){auto id=controller->createSection();QVERIFY(id);QVERIFY(controller->setCutAwayDirection(*id,i%3==0?Vector3d(0,0,-1):i%3==1?Vector3d(0,-1,0):Vector3d(-1,0,0)));ids.push_back(*id);}
  for(int i=0;i<8;++i)QVERIFY(controller->enabled(ids[i]));QVERIFY(!controller->enabled(ids[8]));QVERIFY(!controller->enable(ids[8],true));QVERIFY(controller->feedback().contains("eight"));
  controller->setGlobalEnabled(true);QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);QCOMPARE(controller->clippingState()->planes.size(),size_t(8));
  auto view=window->qglview;view->navigation()->apply({CameraPose::upright(Vector3d::UnitZ()),Vector3d::Zero(),70,45,Camera::ProjectionType::ORTHOGONAL});auto retained=projected(*view,{5,5,10}),discarded=projected(*view,{-5,5,10});QVERIFY(window->selectionService->pick(retained,window->evaluationBinding->stamp()).hit);QVERIFY(!window->selectionService->pick(discarded,window->evaluationBinding->stamp()).hit);
  controller->setGlobalEnabled(false);QVERIFY(controller->clippingState()->planes.empty());QCOMPARE(window->inspectionPresentation->status(),Status::Ready);QVERIFY(window->selectionService->pick(discarded,window->evaluationBinding->stamp()).hit);controller->setGlobalEnabled(true);QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);QCOMPARE(controller->clippingState()->planes.size(),size_t(8));QVERIFY(!window->selectionService->pick(discarded,window->evaluationBinding->stamp()).hit);
  controller->setGlobalEnabled(false);QVERIFY(controller->enable(ids[0],false));QVERIFY(controller->enable(ids[8],true));
  Design::FieldChange offset{Design::ChangeKind::Offset};offset.offset=-50;QVERIFY(controller->edit(ids[1],{offset}));QVERIFY(std::dynamic_pointer_cast<const Design::Section>(controller->records().at(ids[1])->evaluated)->offset==-50);
  QVERIFY(compile(*window,"function placement()={offset:1};\n@{type:Section}\ncomputed={id:\"computed\",origin:[0,0,0],normal:[0,0,1],adjust:placement()};\n@{type:Section}\nmissing={id:\"missing\",origin:[0,0,0],normal:[0,0,1],frame:{domain:\"design\",id:\"gone\"}};\n@{type:Section}\ninvalid={id:\"invalid\",origin:[0,0,0],normal:[0,0,0]};\ncube(10);"));
  QString reason;QVERIFY(!controller->editable("computed",&reason));QVERIFY(!reason.isEmpty());QVERIFY(!controller->edit("computed",{offset}));QVERIFY(!controller->frame("missing",&reason));QVERIFY(!controller->records().count("invalid"));QVERIFY(!controller->enable("missing",true));
  controller->select("computed");controller->dock->show();controller->dock->refresh();QVERIFY(!controller->dock->findChild<QDoubleSpinBox *>("sectionOriginX")->isEnabled());controller->setGlobalEnabled(false);
}
void TestClipping::routedPicksAndGestures() {
  QVERIFY(compile(*window,"difference(){cube(10,center=true);cube(8,center=true);}"));auto controller=Clipping::Controller::forWindow(*window);auto id=controller->createSection();QVERIFY(id);controller->setGlobalEnabled(false);controller->setHelperEnabled(*id,false);
  auto view=window->qglview;view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d::Zero(),40,45,Camera::ProjectionType::ORTHOGONAL});
  auto before=*controller->frame(*id);controller->pick(*id,false);QVERIFY(window->viewportTools->active());QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,{2,2});QVERIFY(window->viewportTools->active());QTest::keyClick(view,Qt::Key_Escape);QVERIFY(!window->viewportTools->active());QVERIFY(controller->frame(*id)->matrix().isApprox(before.matrix()));
  controller->pick(*id,false);QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projected(*view,{0,-5,0}).toPoint());QTRY_VERIFY(!window->viewportTools->active());QVERIFY(controller->frame(*id)->linear().isApprox(before.linear()));QVERIFY(std::abs(controller->frame(*id)->translation().y()+5)<1e-3);
  controller->pick(*id,true);QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projected(*view,{0,-5,0}).toPoint());QTRY_VERIFY(!window->viewportTools->active());auto pose=controller->frame(*id);QVERIFY(pose);QVERIFY(std::abs(pose->translation().y()+5)<1e-3);QVERIFY(pose->linear().col(2).dot(Vector3d::UnitY())>.999);
  auto pivot=pose->translation();QVERIFY(controller->flip(*id));QVERIFY(controller->flip(*id));QVERIFY(controller->frame(*id)->translation().isApprox(pivot,1e-10));QVERIFY(controller->frame(*id)->linear().isApprox(pose->linear(),1e-10));QVERIFY(controller->setCutAwayDirection(*id,Vector3d(1,2,3)));QVERIFY(controller->frame(*id)->translation().isApprox(pivot,1e-10));
  QVERIFY(controller->frame(*id)->linear().col(2).isApprox(-Vector3d(1,2,3).normalized()));
  const auto customPose=*controller->frame(*id);
  QVERIFY(!controller->setCutAwayDirection(*id,Vector3d::Zero()));
  QVERIFY(controller->frame(*id)->matrix().isApprox(customPose.matrix(),1e-10));
  QVERIFY(!controller->setCutAwayDirection(*id,Vector3d(std::numeric_limits<double>::infinity(),0,0)));
  QVERIFY(controller->frame(*id)->matrix().isApprox(customPose.matrix(),1e-10));
  QVERIFY(controller->setCutAwayDirection(*id,Vector3d(10,20,30)));
  QVERIFY(controller->frame(*id)->matrix().isApprox(customPose.matrix(),1e-10));
  QVERIFY(controller->setCutAwayDirection(*id,Vector3d::Constant(std::numeric_limits<double>::max())));
  QVERIFY(controller->frame(*id)->linear().col(2).isApprox(-Vector3d::Ones().normalized()));
  QVERIFY(controller->setCutAwayDirection(*id,Vector3d(0,0,std::numeric_limits<double>::min())));
  QVERIFY(controller->frame(*id)->linear().col(2).isApprox(-Vector3d::UnitZ()));
  QVERIFY(controller->frame(*id)->translation().isApprox(pivot,1e-10));
  controller->setHelperEnabled(*id,true);const auto beforeDrag=*controller->frame(*id);
  const auto marker=projected(*view,pivot).toPoint();QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,marker);
  QTest::mouseMove(view,marker+QPoint(25,10));QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,marker+QPoint(25,10));
  QVERIFY(!window->viewportTools->active());QVERIFY(controller->frame(*id)->matrix().isApprox(beforeDrag.matrix()));
  controller->setGlobalEnabled(false);
}
void TestClipping::sourceDirectionAndOwnership() {
  const QString source="@{type:Section}\ncut={id:\"cut\",origin:[0,0,0],normal:[0,0,1]};\ncube(10);";
  QVERIFY(compile(*window,source));auto controller=Clipping::Controller::forWindow(*window);
  const Vector3d pivot=controller->frame("cut")->translation();
  QVERIFY(controller->setCutAwayDirection("cut",Vector3d(0,0,5)));
  QVERIFY(controller->frame("cut")->linear().col(2).isApprox(-Vector3d::UnitZ()));
  QVERIFY(controller->frame("cut")->translation().isApprox(pivot));
  QVERIFY(window->activeEditor->toPlainText()!=source);
  window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);
  QVERIFY(compile(*window,"cube(10);"));auto id=controller->createSection();QVERIFY(id);
  controller->setGlobalEnabled(false);controller->pick(*id,false);QVERIFY(window->viewportTools->active());
  window->activeEditor->setContentModified(false);window->tabManager->closeCurrentTab();QVERIFY(!window->viewportTools->active());
}
void TestClipping::directionControlsAndDocumentState() {
  QVERIFY(compile(*window,"cube(20,center=true);"));
  auto controller=Clipping::Controller::forWindow(*window);controller->dock->show();
  auto create=controller->dock->findChild<QPushButton *>("sectionNew");QVERIFY(create);create->click();
  const auto id=controller->selectedId();QVERIFY(!id.empty());
  controller->setGlobalEnabled(true);
  QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);
  controller->setGlobalEnabled(false);
  auto preset=controller->dock->findChild<QComboBox *>("sectionDirectionPreset");QVERIFY(preset);
  auto fields=controller->dock->findChild<QWidget *>("sectionDirectionFields");QVERIFY(fields);
  QVERIFY(fields->isHidden());
  QVERIFY(fields->layout()->contentsMargins().left()>0);
  auto splitter=controller->dock->findChild<QSplitter *>("sectionListSplitter");QVERIFY(splitter);
  auto rows=controller->dock->findChild<QTreeWidget *>("sectionRows");QVERIFY(rows);
  QCoreApplication::processEvents();const int listHeight=rows->height();
  QDoubleSpinBox *positions[3];
  for(int axis=0;axis<3;++axis){positions[axis]=controller->dock->findChild<QDoubleSpinBox *>("sectionOrigin"+QString("XYZ").mid(axis,1));QVERIFY(positions[axis]);}
  QLineEdit *components[3];for(int axis=0;axis<3;++axis){components[axis]=controller->dock->findChild<QLineEdit *>("sectionDirection"+QString("XYZ").mid(axis,1));QVERIFY(components[axis]);}
  QCOMPARE(preset->count(),7);QCOMPARE(preset->currentIndex(),0);
  QVERIFY(controller->frame(id)->linear().col(2).isApprox(-Vector3d::UnitZ()));
  const Vector3d origin=controller->frame(id)->translation();
  auto originZ=controller->dock->findChild<QDoubleSpinBox *>("sectionOriginZ");QVERIFY(originZ);
  originZ->setFocus();
  // Spin-button steps must move the plane without finishing the edit or saving.
  originZ->stepUp();QCOMPARE(controller->frame(id)->translation().z(),origin.z()+originZ->singleStep());
  originZ->stepUp();QCOMPARE(controller->frame(id)->translation().z(),origin.z()+2*originZ->singleStep());
  originZ->stepDown();QCOMPARE(controller->frame(id)->translation().z(),origin.z()+originZ->singleStep());
  originZ->stepDown();QVERIFY(controller->frame(id)->translation().isApprox(origin));
  const std::vector<Vector3d> directions={{0,0,1},{0,0,-1},{0,1,0},{0,-1,0},{1,0,0},{-1,0,0}};
  for(int index=0;index<6;++index){
    preset->setCurrentIndex(index);QVERIFY(fields->isHidden());
    const int activeAxis=index<2?2:index<4?1:0;
    for(int axis=0;axis<3;++axis)QCOMPARE(positions[axis]->isHidden(),axis!=activeAxis);
    QVERIFY(controller->frame(id)->linear().col(2).isApprox(-directions[index]));QVERIFY(controller->frame(id)->translation().isApprox(origin));
    for(auto component:components)QVERIFY(!component->isEnabled());
    QCoreApplication::processEvents();QCOMPARE(rows->height(),listHeight);
  }
  preset->setCurrentIndex(6);QVERIFY(!fields->isHidden());for(auto component:components)QVERIFY(component->isEnabled());
  for(auto position:positions)QVERIFY(!position->isHidden());
  QCoreApplication::processEvents();QCOMPARE(rows->height(),listHeight);
  // A user-sized list must remain independent of the controls' content height too.
  auto sizes=splitter->sizes();sizes[0]+=20;sizes[1]-=20;splitter->setSizes(sizes);
  QCoreApplication::processEvents();const int resizedHeight=rows->height();
  QVERIFY(resizedHeight>listHeight);
  preset->setCurrentIndex(0);QCoreApplication::processEvents();QCOMPARE(rows->height(),resizedHeight);
  preset->setCurrentIndex(6);QCoreApplication::processEvents();QCOMPARE(rows->height(),resizedHeight);
  // Finishing one component must not normalize or rewrite the remaining text.
  components[0]->setText("2");components[1]->setText("4");components[2]->setText("6");
  QVERIFY(QMetaObject::invokeMethod(components[0],"editingFinished"));
  QVERIFY(controller->frame(id)->linear().col(2).isApprox(-Vector3d(1,2,3).normalized()));
  QCOMPARE(components[0]->text(),QString("2"));QCOMPARE(components[1]->text(),QString("4"));QCOMPARE(components[2]->text(),QString("6"));
  components[1]->setText("8");QVERIFY(QMetaObject::invokeMethod(components[1],"editingFinished"));
  QVERIFY(controller->frame(id)->linear().col(2).isApprox(-Vector3d(2,8,6).normalized()));
  QCOMPARE(components[0]->text(),QString("2"));QCOMPARE(components[2]->text(),QString("6"));
  const auto beforeZero=*controller->frame(id);for(auto component:components)component->setText("0");
  QVERIFY(QMetaObject::invokeMethod(components[2],"editingFinished"));
  QVERIFY(controller->frame(id)->matrix().isApprox(beforeZero.matrix()));
  for(int axis=0;axis<3;++axis)QCOMPARE(components[axis]->text(),QString::number(-beforeZero.linear().col(2)[axis],'g',17));
  components[0]->setText("0");components[1]->setText("0");
  components[2]->setText("1e-200");QVERIFY(QMetaObject::invokeMethod(components[2],"editingFinished"));
  QVERIFY(controller->frame(id)->linear().col(2).isApprox(-Vector3d::UnitZ()));QCOMPARE(components[2]->text(),QString("1e-200"));
  QVERIFY(controller->flip(id));QCOMPARE(preset->currentIndex(),1);QVERIFY(fields->isHidden());
  preset->setCurrentIndex(6);components[2]->setText("-5");QVERIFY(QMetaObject::invokeMethod(components[2],"editingFinished"));
  const auto firstDocument=controller->documentId();
  window->tabManager->createTab(QString());
  const QString source="@{type:Section}\ncut={id:\"cut\",origin:[0,0,0],normal:[0,0,1]};\ncube(10);";
  QVERIFY(compile(*window,source));controller->select("cut");
  QVERIFY(controller->documentId()!=firstDocument);QCOMPARE(preset->currentIndex(),1);
  QCOMPARE(components[2]->text(),QString("-1"));
  controller->setGlobalEnabled(false);
}
void TestClipping::positionSpinButtons() {
  QVERIFY(compile(*window,"cube(20,center=true);"));
  auto controller=Clipping::Controller::forWindow(*window);controller->dock->show();
  auto id=controller->createSection();QVERIFY(id);controller->setGlobalEnabled(true);
  QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);
  auto spin=controller->dock->findChild<QDoubleSpinBox *>("sectionOriginZ");QVERIFY(spin);
  QTRY_VERIFY(spin->isVisible() && spin->isEnabled());
  QStyleOptionSpinBox option;option.initFrom(spin);option.frame=true;option.buttonSymbols=QAbstractSpinBox::UpDownArrows;
  option.stepEnabled=QAbstractSpinBox::StepUpEnabled|QAbstractSpinBox::StepDownEnabled;
  const auto up=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxUp,spin).center();
  for(int step=1;step<=3;++step){
    QTest::mouseClick(spin,Qt::LeftButton,Qt::NoModifier,up);
    qInfo("Origin button: widget %.17g, plane %.17g, enabled %d, feedback %s",spin->value(),controller->frame(*id)->translation().z(),spin->isEnabled(),qPrintable(controller->feedback()));
    QCOMPARE(spin->value(),double(step));QCOMPARE(controller->frame(*id)->translation().z(),double(step));
    QCOMPARE(controller->clippingState()->planes.size(),size_t(1));QCOMPARE(controller->clippingState()->planes.front().origin.z(),double(step));
  }
  controller->setGlobalEnabled(false);
}
void TestClipping::emptyDocumentSection() {
  QVERIFY(window->activeEditor->toPlainText().isEmpty());
  window->evaluationBinding->invalidate("Empty-document fixture before first evaluation");
  QVERIFY(!window->evaluationBinding->current());
  auto controller=Clipping::Controller::forWindow(*window);controller->dock->show();
  auto create=controller->dock->findChild<QPushButton *>("sectionNew");QVERIFY(create);create->click();
  const auto id=controller->selectedId();QVERIFY(!id.empty());
  QVERIFY(controller->frame(id));QVERIFY(controller->frame(id)->translation().isZero());
  QVERIFY(controller->feedback().isEmpty());QVERIFY(controller->editable(id));
  auto spin=controller->dock->findChild<QDoubleSpinBox *>("sectionOriginZ");QVERIFY(spin && spin->isEnabled());
  spin->stepUp();QCOMPARE(spin->value(),1.);QCOMPARE(controller->frame(id)->translation().z(),1.);
  window->activeEditor->undo();QCOMPARE(controller->frame(id)->translation().z(),0.);
  window->activeEditor->redo();QCOMPARE(controller->frame(id)->translation().z(),1.);
  QVERIFY(!window->evaluationBinding->current());QVERIFY(window->activeEditor->toPlainText().isEmpty());
  QVERIFY(!controller->dock->findChild<QPushButton *>("sectionPickPoint")->isEnabled());
  QVERIFY(compile(*window,"cube(10,center=true);"));
  QCOMPARE(controller->frame(id)->translation().z(),1.);
  controller->setGlobalEnabled(true);
  QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);
  QCOMPARE(controller->clippingState()->planes.size(),size_t(1));
  controller->setGlobalEnabled(false);
  QVERIFY(controller->remove(id));QVERIFY(!controller->records().count(id));
}
void TestClipping::openExampleAfterSection() {
  QVERIFY(window->activeEditor->toPlainText().isEmpty());
  auto controller=Clipping::Controller::forWindow(*window);controller->dock->show();
  controller->setGlobalEnabled(true);
  auto create=controller->dock->findChild<QPushButton *>("sectionNew");QVERIFY(create);create->click();
  const auto id=controller->selectedId();QVERIFY(!id.empty());
  auto editor=window->activeEditor;const auto oldDocument=editor->documentId();
  const auto example=QCoreApplication::applicationDirPath()+"/examples/Basics/CSG.scad";
  QVERIFY(QFileInfo::exists(example));window->tabManager->open(example);
  QCOMPARE(window->activeEditor,editor);
  qInfo("Opened example; identity renewed=%d; current=%d; section retained=%d",oldDocument!=editor->documentId(),controller->evaluationCurrent(),controller->records().count(id)!=0);
  QVERIFY(compile(*window,editor->toPlainText()));
  qInfo("After preview: selected=%s expected=%s",controller->selectedId().c_str(),id.c_str());
  controller->setHelperEnabled(id,true);QVERIFY(controller->enable(id,true));controller->setGlobalEnabled(true);
  QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);
  auto spin=controller->dock->findChild<QDoubleSpinBox *>("sectionOriginZ");QVERIFY(spin);
  QTRY_VERIFY(spin->isEnabled());
  // The normal user's Editor dock may be hidden, making its widget read-only.
  window->editorDock->hide();QVERIFY(!window->activeEditor->isSourceReadOnly());
  spin->setFocus();QTest::keyClick(spin,Qt::Key_A,Qt::ControlModifier);QTest::keyClicks(spin,"3");QTest::keyClick(spin,Qt::Key_Return);
  qInfo("After typed Z: widget %.17g, plane %.17g, feedback %s",spin->value(),controller->frame(id)->translation().z(),qPrintable(controller->feedback()));
  QCOMPARE(spin->value(),3.);QCOMPARE(controller->frame(id)->translation().z(),3.);
  spin->stepUp();QCOMPARE(spin->value(),4.);QCOMPARE(controller->frame(id)->translation().z(),4.);
  controller->setGlobalEnabled(false);
}
void TestClipping::worldOffsetPoseMapping() {
  const QString source="@{type:Section}\ncut={id:\"cut\",origin:[1,2,3],normal:[0,0,1],up:[0,1,0],adjust:{offset:7,translation:[4,5,6],rotation:[0,0,0,1]}};\ncube(50);";
  QVERIFY(compile(*window,source));auto controller=Clipping::Controller::forWindow(*window);controller->setGlobalEnabled(false);controller->select("cut");
  const auto before=*controller->frame("cut");
  QVERIFY(before.translation().isApprox(Vector3d(5,7,16)));
  const std::vector<Vector3d> directions={{0,0,1},{0,0,-1},{1,2,3}};
  for(const auto& direction:directions){
    QVERIFY(controller->setCutAwayDirection("cut",direction));
    const auto pose=*controller->frame("cut");QVERIFY(pose.translation().isApprox(before.translation(),1e-10));
    QVERIFY(pose.linear().col(2).isApprox(-direction.normalized(),1e-10));
    auto section=std::dynamic_pointer_cast<const Design::Section>(controller->records().at("cut")->evaluated);QVERIFY(section);QCOMPARE(section->offset,7.);
    QVERIFY(window->activeEditor->toPlainText().contains("origin:[1,2,3]"));
    window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);
    QVERIFY(compile(*window,source));QVERIFY(controller->frame("cut")->matrix().isApprox(before.matrix(),1e-10));
  }
  const Vector3d moved(33,-14,8);QVERIFY(controller->setOrigin("cut",moved));
  QVERIFY(controller->frame("cut")->translation().isApprox(moved,1e-10));
  QVERIFY(controller->frame("cut")->linear().isApprox(before.linear(),1e-10));
  auto originX=controller->dock->findChild<QDoubleSpinBox *>("sectionOriginX");QVERIFY(originX);QCOMPARE(originX->value(),33.);
  window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);
}
void TestClipping::cavityAndTransformedPickAlignment() {
  struct Fixture{QString source;Vector3d camera,point,normal;};
  const std::vector<Fixture> fixtures={
    {"difference(){cube(10,center=true);cube(8,center=true);}",{0,-1,0},{0,4,0},{0,-1,0}},
    {"rotate([0,0,30])scale([2,1,.5])cube(10,center=true);",{.5,-std::sqrt(3.)/2,0},{2.5,-4.330127018922,0},{.5,-std::sqrt(3.)/2,0}},
    {"mirror([1,0,0])rotate([0,0,30])scale([2,1,.5])cube(10,center=true);",{-.5,-std::sqrt(3.)/2,0},{-2.5,-4.330127018922,0},{-.5,-std::sqrt(3.)/2,0}}
  };
  auto controller=Clipping::Controller::forWindow(*window);
  for(size_t index=0;index<fixtures.size();++index){const auto& fixture=fixtures[index];controller->setGlobalEnabled(false);QVERIFY(compile(*window,fixture.source));auto id=controller->createSection();QVERIFY(id);QVERIFY(controller->setCutAwayDirection(*id,-Vector3d::UnitY()));controller->setHelperEnabled(*id,false);controller->setGlobalEnabled(index==0);if(index==0)QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);
    auto view=window->qglview;view->navigation()->apply({CameraPose::upright(fixture.camera),-fixture.point,50,45,Camera::ProjectionType::ORTHOGONAL});auto click=projected(*view,fixture.point).toPoint();auto expected=window->selectionService->pick(click,window->evaluationBinding->stamp());QVERIFY(expected.hit);QVERIFY(expected.hit->outwardNormal.dot(fixture.normal)>.999);
    controller->pick(*id,true);QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,click);QTRY_VERIFY(!window->viewportTools->active());auto pose=controller->frame(*id);QVERIFY(pose);QVERIFY(pose->translation().isApprox(expected.hit->worldPoint,1e-5));QVERIFY(pose->linear().col(2).dot(-expected.hit->outwardNormal)>.999);controller->setGlobalEnabled(false);
  }
}
