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
#include <QPushButton>
#include <QLineF>
#include "gui/QSettingsCached.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QWindow>
#include <algorithm>

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
class MouseProbe : public QObject {
public:
  int moves=0;QPointF last;
  bool eventFilter(QObject *,QEvent *event) override {if(event->type()==QEvent::MouseMove){++moves;last=static_cast<QMouseEvent *>(event)->position();}return false;}
};
void nativeMove(QGLView& view,QPoint point) {
  QTest::mouseMove(view.window()->windowHandle(),view.mapTo(view.window(),point));
}
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
    QTRY_VERIFY_WITH_TIMEOUT(presentation->status()!=Status::Preparing,20000);
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
  auto high=presentation->registerMeshProvider("piece-fixture",100,&owner,[supplied](auto,auto,auto done){done({Status::Ready,supplied,{}});return InspectionPresentationController::Cancel{};});
  Effect effect(*window);auto request=presentation->requestMeshes("piece-owner",&owner);
  QTRY_VERIFY(presentation->status()==Status::Ready);
  QVERIFY(request.setMask({1,{77}},presentation->stamp(),22));
  effect.planes({{{0,14,0},{0,1,0}}});auto view=window->qglview;
  view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d(0,-19,0),50,45,Camera::ProjectionType::ORTHOGONAL});
  auto picked=window->selectionService->pick(projected(*view,{0,19,0}),window->evaluationBinding->stamp());QVERIFY(picked.hit);QCOMPARE(picked.hit->object.drawId,uint64_t(77));QVERIFY(std::abs(picked.hit->worldPoint.y()-19)<1e-4);QVERIFY(picked.hit->outwardNormal.dot(Vector3d::UnitY())>.999);
  effect.effect.reset();QVERIFY(presentation->meshes()==supplied);
  picked=window->selectionService->pick(projected(*view,{0,9,0}),window->evaluationBinding->stamp());QVERIFY(picked.hit);QCOMPARE(picked.hit->object.drawId,uint64_t(77));QVERIFY(std::abs(picked.hit->worldPoint.y()-9)<1e-4);
  high.reset();auto failed=presentation->registerMeshProvider("failed-pieces",100,&owner,[](auto,auto,auto done){done({Status::Failed,{},"fixture failure"});return InspectionPresentationController::Cancel{};});
  QTRY_VERIFY(presentation->status()==Status::Failed);QCOMPARE(root.metrics().jobs.load(),uint64_t(0));
  failed.reset(); auto pending=presentation->registerMeshProvider("pending-pieces",100,&owner,[](auto,auto,auto){return InspectionPresentationController::Cancel{};});
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
    QObject owner;auto provider=presentation->registerMeshProvider("numerical-sheets",100,&owner,[set](auto,auto,auto done){done({Status::Ready,set,{}});return InspectionPresentationController::Cancel{};});
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
  auto offset=controller->dock->findChild<QDoubleSpinBox *>("sectionOffset");QVERIFY(offset);QVERIFY(offset->isEnabled());
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
  auto id=controller->createPreset("YZ");QVERIFY(id);QVERIFY(controller->edit(*id,{{Design::ChangeKind::Label,"Named plane"}}));auto before=window->activeEditor->toPlainText();QVERIFY(controller->promote(*id));QVERIFY(window->activeEditor->toPlainText()!=before);QVERIFY(compile(*window,window->activeEditor->toPlainText()));QVERIFY(controller->records().at(*id)->source);
  QTemporaryDir directory;QVERIFY(directory.isValid());auto path=directory.filePath("source-sections.scad");QVERIFY(window->tabManager->saveAs(window->activeEditor,path));QVERIFY(compile(*window,window->activeEditor->toPlainText()));
  QVERIFY(controller->enable(*id,true));controller->setHelperEnabled(*id,true);auto stableId=*id;window->tabManager->closeCurrentTab();window->tabManager->open(path);QVERIFY(compile(*window,window->activeEditor->toPlainText()));QVERIFY(controller->records().count(stableId));QVERIFY(controller->enabled(stableId));QVERIFY(controller->helperEnabled(stableId));QCOMPARE(controller->records().at(stableId)->label,std::string("Named plane"));
  QVERIFY(!QFile::exists(path+".sections.json"));controller->setGlobalEnabled(false);
}
void TestClipping::deleteAndSessionPersistence() {
  QVERIFY(compile(*window,"cube(20,center=true);"));
  auto controller=Clipping::Controller::forWindow(*window);controller->dock->show();
  auto id=controller->createPreset("XY");QVERIFY(id);controller->setGlobalEnabled(false);
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
  std::vector<std::string> ids;for(int i=0;i<9;++i){auto id=controller->createPreset(i%3==0?"XY":i%3==1?"XZ":"YZ");QVERIFY(id);ids.push_back(*id);}
  for(int i=0;i<8;++i)QVERIFY(controller->enabled(ids[i]));QVERIFY(!controller->enabled(ids[8]));QVERIFY(!controller->enable(ids[8],true));QVERIFY(controller->feedback().contains("eight"));
  controller->setGlobalEnabled(true);QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);QCOMPARE(controller->clippingState()->planes.size(),size_t(8));
  auto view=window->qglview;view->navigation()->apply({CameraPose::upright(Vector3d::UnitZ()),Vector3d::Zero(),70,45,Camera::ProjectionType::ORTHOGONAL});auto retained=projected(*view,{5,5,10}),discarded=projected(*view,{-5,5,10});QVERIFY(window->selectionService->pick(retained,window->evaluationBinding->stamp()).hit);QVERIFY(!window->selectionService->pick(discarded,window->evaluationBinding->stamp()).hit);
  controller->setGlobalEnabled(false);QVERIFY(controller->clippingState()->planes.empty());QCOMPARE(window->inspectionPresentation->status(),Status::Inactive);QVERIFY(window->selectionService->pick(discarded,window->evaluationBinding->stamp()).hit);controller->setGlobalEnabled(true);QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);QCOMPARE(controller->clippingState()->planes.size(),size_t(8));QVERIFY(!window->selectionService->pick(discarded,window->evaluationBinding->stamp()).hit);
  controller->setGlobalEnabled(false);QVERIFY(controller->enable(ids[0],false));QVERIFY(controller->enable(ids[8],true));
  Design::FieldChange offset{Design::ChangeKind::Offset};offset.offset=-50;QVERIFY(controller->edit(ids[1],{offset}));auto range=controller->offsetRange(ids[1]);QVERIFY(range && range->first<0 && range->second>0);QVERIFY(std::dynamic_pointer_cast<const Design::Section>(controller->records().at(ids[1])->evaluated)->offset==-50);
  QVERIFY(compile(*window,"function placement()={offset:1};\n@{type:Section}\ncomputed={id:\"computed\",origin:[0,0,0],normal:[0,0,1],adjust:placement()};\n@{type:Section}\nmissing={id:\"missing\",origin:[0,0,0],normal:[0,0,1],frame:{domain:\"design\",id:\"gone\"}};\n@{type:Section}\ninvalid={id:\"invalid\",origin:[0,0,0],normal:[0,0,0]};\ncube(10);"));
  QString reason;QVERIFY(!controller->editable("computed",&reason));QVERIFY(!reason.isEmpty());QVERIFY(!controller->edit("computed",{offset}));QVERIFY(!controller->frame("missing",&reason));QVERIFY(!controller->records().count("invalid"));QVERIFY(!controller->enable("missing",true));
  controller->select("computed");controller->dock->show();controller->dock->refresh();QVERIFY(!controller->dock->findChild<QDoubleSpinBox *>("sectionOffset")->isEnabled());controller->setGlobalEnabled(false);
}
void TestClipping::routedPicksAndGestures() {
  QVERIFY(compile(*window,"difference(){cube(10,center=true);cube(8,center=true);}"));auto controller=Clipping::Controller::forWindow(*window);auto id=controller->createPreset("XY");QVERIFY(id);controller->setGlobalEnabled(false);controller->setHelperEnabled(*id,false);
  auto view=window->qglview;view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d::Zero(),40,45,Camera::ProjectionType::ORTHOGONAL});
  auto before=*controller->frame(*id);controller->pick(*id,false);QVERIFY(window->viewportTools->active());QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,{2,2});QVERIFY(window->viewportTools->active());QTest::keyClick(view,Qt::Key_Escape);QVERIFY(!window->viewportTools->active());QVERIFY(controller->frame(*id)->matrix().isApprox(before.matrix()));
  controller->pick(*id,false);QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projected(*view,{0,-5,0}).toPoint());QTRY_VERIFY(!window->viewportTools->active());QVERIFY(controller->frame(*id)->linear().isApprox(before.linear()));QVERIFY(std::abs(controller->frame(*id)->translation().y()+5)<1e-3);
  controller->pick(*id,true);QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projected(*view,{0,-5,0}).toPoint());QTRY_VERIFY(!window->viewportTools->active());auto pose=controller->frame(*id);QVERIFY(pose);QVERIFY(std::abs(pose->translation().y()+5)<1e-3);QVERIFY(pose->linear().col(2).dot(Vector3d::UnitY())>.999);
  auto pivot=pose->translation();QVERIFY(controller->flip(*id));QVERIFY(controller->flip(*id));QVERIFY(controller->frame(*id)->translation().isApprox(pivot,1e-10));QVERIFY(controller->frame(*id)->linear().isApprox(pose->linear(),1e-10));QVERIFY(controller->setAngles(*id,30,20));QVERIFY(controller->frame(*id)->translation().isApprox(pivot,1e-10));
  auto initial=*controller->frame(*id);Vector3d up=initial.linear().col(1);view->navigation()->apply({CameraPose::upright(-up),-pivot,40,45,Camera::ProjectionType::ORTHOGONAL});
  auto start=projected(*view,pivot+initial.linear().col(2)*2),finish=projected(*view,pivot+initial.linear().col(2)*7);
  const auto jobs=controller->metrics().jobs.load();QSignalSpy evaluations(window,&MainWindow::compilationDone);
  QVERIFY(controller->beginDrag(*id,start,false));for(int i=0;i<100;++i)QTest::mouseMove(view,(start+(finish-start)*i/99.).toPoint());QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,finish.toPoint());QVERIFY(!window->viewportTools->active());qInfo("Section drag displacement %.9g, projected pixels %.9g, viewport %d x %d, DPR %.3f",(controller->frame(*id)->translation()-pivot).dot(initial.linear().col(2)),QLineF(start,finish).length(),view->width(),view->height(),view->devicePixelRatio());QVERIFY((controller->frame(*id)->translation()-pivot).dot(initial.linear().col(2))>4.5);QCOMPARE(controller->metrics().jobs.load(),jobs);QVERIFY(evaluations.empty());
  auto moved=*controller->frame(*id);QVERIFY(controller->beginDrag(*id,start,false));QTest::mouseMove(view,finish.toPoint());QTest::keyClick(view,Qt::Key_Escape);QVERIFY(controller->frame(*id)->matrix().isApprox(moved.matrix(),1e-10));
  QVERIFY(controller->beginDrag(*id,start,false));auto external=window->viewportTools->claimExternal("measurement fixture",window,[](auto){});QVERIFY(external);QVERIFY(controller->frame(*id)->matrix().isApprox(moved.matrix(),1e-10));window->viewportTools->finish(external);
  QVERIFY(controller->beginDrag(*id,start,false));window->activeEditor->insert("\n// stale");QVERIFY(!window->viewportTools->active());controller->setGlobalEnabled(false);
}
void TestClipping::sourceManipulatorRotationAndOwnership() {
  Clipping::Controller::forWindow(*window)->setHandlesEnabled(true);
  const QString source="height=40;\n@{type:Section}\ncut={id:\"cut\",origin:[0,0,height/2],normal:[0,0,1],up:[0,1,0],adjust:{offset:0}};\ncube([20,20,height],center=false);";
  QVERIFY(compile(*window,source));auto controller=Clipping::Controller::forWindow(*window);controller->select("cut");QVERIFY(controller->enable("cut",true));controller->setGlobalEnabled(true);QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);controller->setHelperEnabled("cut",false);
  auto view=window->qglview;Vector3d pivot=controller->frame("cut")->translation();view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),-pivot,70*view->height()/538.,45,Camera::ProjectionType::ORTHOGONAL});
  double handleSize=controller->bounds().sizes().norm()*.18;controller->setHelperEnabled("cut",true);
  auto start=projected(*view,pivot),end=projected(*view,pivot+Vector3d(0,0,12));
  const auto jobs=controller->metrics().jobs.load(),uploads=window->inspectionPresentation->meshUploadCount();QVERIFY(uploads>0);QSignalSpy evaluations(window,&MainWindow::compilationDone),resizes(view,&QGLView::resized),cameras(view->navigation(),&CameraNavigation::displayChanged);
  QTest::qWait(30);start=projected(*view,pivot);end=projected(*view,pivot+Vector3d(0,0,12));
  MouseProbe probe;view->installEventFilter(&probe);QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,start.toPoint());QVERIFY(window->viewportTools->active());for(int i=0;i<110;++i)nativeMove(*view,(start+(end-start)*i/109.).toPoint());view->removeEventFilter(&probe);qInfo("Source move events %d last %.6g %.6g",probe.moves,probe.last.x(),probe.last.y());QVERIFY(probe.moves>=100);
  auto document=Design::DesignDocument::forEditor(*window->activeEditor,*window);for(const auto& [token,draft]:document->drafts())qInfo("Source draft changes %zu preview offset %.10g",draft.changes.size(),std::dynamic_pointer_cast<const Design::Section>(draft.preview->evaluated)->offset);
  qInfo("Source drag active %llu resizes %d camera changes %d origin z %.10g start %.4g %.4g end %.4g %.4g viewport %d %d status %s",window->viewportTools->active(),int(resizes.size()),int(cameras.size()),controller->frame("cut")->translation().z(),start.x(),start.y(),end.x(),end.y(),view->width(),view->height(),qPrintable(controller->feedback()));QCOMPARE(window->activeEditor->toPlainText(),source);QVERIFY(controller->frame("cut")->translation().z()>pivot.z()+3.8);QCOMPARE(controller->metrics().jobs.load(),jobs);QCOMPARE(window->inspectionPresentation->meshUploadCount(),uploads);QVERIFY(evaluations.empty());
  QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,end.toPoint());QVERIFY(window->activeEditor->toPlainText().contains("height/2"));QVERIFY(window->activeEditor->toPlainText()!=source);window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);QVERIFY(compile(*window,source));QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);
  auto initial=*controller->frame("cut");Vector3d axis=initial.linear().col(0),normal=initial.linear().col(2),up=initial.linear().col(1);pivot=initial.translation();view->navigation()->apply({CameraPose::upright(-axis),-pivot,70,45,Camera::ProjectionType::ORTHOGONAL});
  double size=controller->bounds().sizes().norm()*.18,angle=26*std::acos(-1.)/180;Vector3d startVector=(normal+up).normalized();start=projected(*view,pivot+size*startVector);Vector3d rotated=Eigen::AngleAxisd(angle,axis)*startVector;end=projected(*view,pivot+size*rotated);
  QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,start.toPoint());QVERIFY(window->viewportTools->active());nativeMove(*view,end.toPoint());QTest::mouseRelease(view,Qt::LeftButton,Qt::ShiftModifier,end.toPoint());QVERIFY(window->activeEditor->toPlainText().contains("height/2"));QVERIFY(compile(*window,window->activeEditor->toPlainText()));QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);
  auto rotatedPose=*controller->frame("cut");QVERIFY(rotatedPose.translation().isApprox(pivot,1e-10));double degrees=std::acos(std::clamp(rotatedPose.linear().col(2).dot(normal),-1.,1.))*180/std::acos(-1.);QVERIFY(std::abs(degrees-30)<1e-8);
  window->activeEditor->undo();QCOMPARE(window->activeEditor->toPlainText(),source);QVERIFY(compile(*window,source));QTRY_VERIFY(window->inspectionPresentation->status()==Status::Ready);
  view->navigation()->apply({CameraPose::upright(-Vector3d::UnitZ()),-pivot,70,45,Camera::ProjectionType::ORTHOGONAL});QVERIFY(!controller->beginDrag("cut",projected(*view,pivot),false));QVERIFY(window->activeEditor->toPlainText()==source);
  view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),-pivot,70,45,Camera::ProjectionType::ORTHOGONAL});start=projected(*view,pivot);QVERIFY(controller->beginDrag("cut",start,false));auto camera=view->navigation()->current();camera.distance*=1.1;view->navigation()->apply(camera);QVERIFY(!window->viewportTools->active());QCOMPARE(window->activeEditor->toPlainText(),source);
  QVERIFY(controller->beginDrag("cut",start,false));window->tabManager->actionNew();QVERIFY(!window->viewportTools->active());controller->setGlobalEnabled(false);
  QVERIFY(compile(*window,"cube(10);"));auto id=controller->createPreset("XY");QVERIFY(id);controller->setGlobalEnabled(false);controller->pick(*id,false);QVERIFY(window->viewportTools->active());window->activeEditor->setContentModified(false);window->tabManager->closeCurrentTab();QVERIFY(!window->viewportTools->active());
}
void TestClipping::cavityAndTransformedPickAlignment() {
  struct Fixture{QString source;Vector3d camera,point,normal;};
  const std::vector<Fixture> fixtures={
    {"difference(){cube(10,center=true);cube(8,center=true);}",{0,-1,0},{0,4,0},{0,-1,0}},
    {"rotate([0,0,30])scale([2,1,.5])cube(10,center=true);",{.5,-std::sqrt(3.)/2,0},{2.5,-4.330127018922,0},{.5,-std::sqrt(3.)/2,0}},
    {"mirror([1,0,0])rotate([0,0,30])scale([2,1,.5])cube(10,center=true);",{-.5,-std::sqrt(3.)/2,0},{-2.5,-4.330127018922,0},{-.5,-std::sqrt(3.)/2,0}}
  };
  auto controller=Clipping::Controller::forWindow(*window);
  for(size_t index=0;index<fixtures.size();++index){const auto& fixture=fixtures[index];controller->setGlobalEnabled(false);QVERIFY(compile(*window,fixture.source));auto id=controller->createPreset("XZ");QVERIFY(id);controller->setHelperEnabled(*id,false);controller->setGlobalEnabled(index==0);if(index==0)QTRY_VERIFY_WITH_TIMEOUT(window->inspectionPresentation->status()==Status::Ready,20000);
    auto view=window->qglview;view->navigation()->apply({CameraPose::upright(fixture.camera),-fixture.point,50,45,Camera::ProjectionType::ORTHOGONAL});auto click=projected(*view,fixture.point).toPoint();auto expected=window->selectionService->pick(click,window->evaluationBinding->stamp());QVERIFY(expected.hit);QVERIFY(expected.hit->outwardNormal.dot(fixture.normal)>.999);
    controller->pick(*id,true);QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,click);QTRY_VERIFY(!window->viewportTools->active());auto pose=controller->frame(*id);QVERIFY(pose);QVERIFY(pose->translation().isApprox(expected.hit->worldPoint,1e-5));QVERIFY(pose->linear().col(2).dot(-expected.hit->outwardNormal)>.999);controller->setGlobalEnabled(false);
  }
}
