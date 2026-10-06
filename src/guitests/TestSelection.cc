#include "TestSelection.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/annotations/AnnotationStore.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/InspectionPresentationController.h"
#include "glview/InspectionMeshRenderer.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/selection/ViewportOverlays.h"
#include "gui/selection/InspectorDock.h"
#include <QLabel>
#include "geometry/PolySetUtils.h"
#include "glview/SurfacePick.h"
#include "glview/PolySetRenderer.h"
#include "glview/RenderSettings.h"
#include "glview/ColorMap.h"
#include "geometry/GeometryEvaluator.h"
#include "core/Tree.h"
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QApplication>
#include <QMessageBox>
#include <QAbstractButton>
#include <future>
#include <algorithm>
#include <cmath>

namespace {
bool writeSource(const QString& path, const QByteArray& bytes)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
BoundingBox evaluatedBounds(const std::shared_ptr<const Selection::EvaluatedScene>& scene)
{
  Tree tree(scene->root, scene->documentPath);
  GeometryEvaluator evaluator(tree);
  auto geometry = evaluator.evaluateGeometry(*scene->root, false);
  return geometry ? geometry->getBoundingBox() : BoundingBox();
}
void discardFixtureDialogs(QTimer& timer)
{
  QObject::connect(&timer, &QTimer::timeout, [] {
    auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (box && box->standardButtons() == (QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel))
      box->button(QMessageBox::Discard)->click();
  });
  timer.start(10);
}

struct SurfaceFixture { const char *name, *source; Vector3d point, normal; };
const std::vector<SurfaceFixture> surfaceFixtures = {
  {"cube", "cube(10,center=true);", {0,-5,0}, {0,-1,0}},
  {"subtraction wall", "difference(){cube(10,center=true); translate([0,-4,0]) cube([4,6,4],center=true);}", {0,-1,0}, {0,-1,0}},
  {"rotation and nonuniform scale", "rotate([0,0,30]) scale([2,1,0.5]) cube(10,center=true);", {2.5,-4.330127018922193,0}, {0.5,-0.8660254037844386,0}},
  {"mirror", "mirror([1,0,0]) rotate([0,0,30]) scale([2,1,0.5]) cube(10,center=true);", {-2.5,-4.330127018922193,0}, {-0.5,-0.8660254037844386,0}},
  {"repeated cubes", "for(x=[-8,8]) translate([x,0,0]) cube(10,center=true);", {8,-5,0}, {0,-1,0}},
  {"overlapping front and back", "translate([0,4,0]) cube(10,center=true); translate([0,-4,0]) cube(10,center=true);", {0,-9,0}, {0,-1,0}},
  {"transparent surface", "color([1,0,0,0.25]) cube(10,center=true);", {0,-5,0}, {0,-1,0}}
};

QPointF projectedPixel(QGLView& view, const Vector3d& world)
{
  view.makeCurrent(); view.setupCamera();
  const Eigen::Map<const Eigen::Matrix4d> modelview(view.modelview), projection(view.projection);
  Eigen::Vector4d point = projection*modelview*Eigen::Vector4d(world.x(),world.y(),world.z(),1);
  point /= point.w();
  view.doneCurrent();
  return {((point.x()+1)*view.cam.pixel_width/2)/view.devicePixelRatio(),
          ((1-point.y())*view.cam.pixel_height/2)/view.devicePixelRatio()};
}

void assertSurface(MainWindow *window, const SurfaceFixture& fixture, bool final)
{
  qInfo("Fixture: %s, final=%d", fixture.name, final);
  window->activeEditor->setPlainText(QString::fromUtf8(fixture.source));
  QSignalSpy completed(window, &MainWindow::compilationDone);
  if (final) window->designActionRender->trigger(); else window->actionRenderPreview();
  QTRY_VERIFY_WITH_TIMEOUT(!completed.empty(), 15000);
  auto binding = window->evaluationBinding;
  QVERIFY2(binding->current(), qPrintable(binding->staleReason()));
  auto view = window->qglview;
  for (auto projection : {Camera::ProjectionType::ORTHOGONAL, Camera::ProjectionType::PERSPECTIVE}) {
    CameraPose pose{CameraPose::upright(fixture.normal), -fixture.point, 50, 45, projection};
    view->navigation()->apply(pose);
    view->update(); QTest::qWait(20);
    const auto logical = projectedPixel(*view, fixture.point);
    const auto physical = SurfacePickMath::pixel(logical.x(),logical.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height);
    QVERIFY(physical);
    const Eigen::Map<const Eigen::Matrix4d> modelview(view->modelview), projectionMatrix(view->projection);
    const auto rayNear = SurfacePickMath::unproject(physical->x(),physical->y(),0,view->cam.pixel_width,view->cam.pixel_height,projectionMatrix,modelview,Vector3d::Zero());
    const auto rayFar = SurfacePickMath::unproject(physical->x(),physical->y(),0.9999,view->cam.pixel_width,view->cam.pixel_height,projectionMatrix,modelview,Vector3d::Zero());
    QVERIFY(rayNear && rayFar);
    const Vector3d ray = *rayFar-*rayNear;
    const Vector3d expected = *rayNear + ray*(fixture.normal.dot(fixture.point-*rayNear)/fixture.normal.dot(ray));
    const auto reference = view->grabFrame(QGLView::CaptureContent::ModelOnly);
    QCOMPARE(reference.width(),view->cam.pixel_width); QCOMPARE(reference.height(),view->cam.pixel_height);
    const QPoint screenSample(physical->x(),view->cam.pixel_height-1-physical->y());
    QVERIFY(reference.pixelColor(screenSample)!=reference.pixelColor(0,0));
    const auto stamp = binding->stamp();
    const auto picked = window->selectionService->pick(logical, stamp);
    QVERIFY2(picked.error == Selection::PickError::None && picked.hit, qPrintable(picked.detail));
    const auto& hit = *picked.hit;
    const auto diagonal = view->renderer->getBoundingBox().diagonal().norm();
    const double pointError = (hit.worldPoint-expected).norm();
    const double normalError = std::acos(std::clamp(hit.outwardNormal.dot(fixture.normal),-1.0,1.0))*180/3.141592653589793;
    qInfo("Point error %.10g, normal error %.10g degrees, DPR %.3f", pointError, normalError, view->devicePixelRatio());
    QVERIFY(pointError <= 1e-5*diagonal);
    QVERIFY(normalError <= 0.1);
    if (!final) QVERIFY(hit.localPoint && hit.localToWorld);
    else QVERIFY(!hit.localPoint && !hit.localToWorld);
    QVERIFY(window->selectionService->state().bounds);
    QVERIFY(window->selectionService->state().bounds->exteriorDistance(hit.worldPoint) <= 1e-5*diagonal);
    QVERIFY(window->selectionService->selectObject(hit.object, stamp));
    QVERIFY(window->selectionService->pick(logical, stamp).hit);
    QCOMPARE(view->grabFrame(QGLView::CaptureContent::ModelOnly), reference);
    QCOMPARE(window->selectionService->pick({-1,-1}, stamp).error, Selection::PickError::InvalidViewport);
  }
}

class OriginRenderer : public Renderer {
public:
  OriginRenderer(std::shared_ptr<const Geometry> geometry, Vector3d origin) : mesh(geometry), origin(origin) {}
  void prepare(const ShaderUtils::ShaderInfo *shader) override { mesh.prepare(shader); }
  void draw(bool edges, const ShaderUtils::ShaderInfo *shader) const override { mesh.draw(edges,shader); }
  BoundingBox getBoundingBox() const override { return mesh.getBoundingBox(); }
  SurfaceContext prepareSurfacePick(const ShaderUtils::ShaderInfo *shader) override {
    auto context=mesh.prepareSurfacePick(shader); context.positionOrigin=origin; return context;
  }
private:
  PolySetRenderer mesh;
  Vector3d origin;
};
struct EffectProbe { bool blocked=false; int colors=0,picks=0; };
class ProbeEffect : public Renderer {
public:
  ProbeEffect(std::shared_ptr<Renderer> child,std::shared_ptr<EffectProbe> probe) : child(std::move(child)),probe(std::move(probe)) {}
  void prepare(const ShaderUtils::ShaderInfo *shader) override { child->prepare(shader); }
  void draw(bool edges,const ShaderUtils::ShaderInfo *shader) const override {
    if (shader && shader->type==ShaderUtils::ShaderType::SURFACE_PICK) ++probe->picks; else ++probe->colors;
    if (!probe->blocked) child->draw(edges,shader);
  }
  BoundingBox getBoundingBox() const override { return child->getBoundingBox(); }
  SurfaceContext prepareSurfacePick(const ShaderUtils::ShaderInfo *shader) override { return child->prepareSurfacePick(shader); }
private:
  std::shared_ptr<Renderer> child;
  std::shared_ptr<EffectProbe> probe;
};
std::shared_ptr<InspectionMeshSet> inspectionFixture(MainWindow *window,const Selection::Stamp& stamp)
{
  auto scene=window->evaluationBinding->scene(stamp);
  Tree tree(scene->root,scene->documentPath); GeometryEvaluator evaluator(tree);
  auto mesh=PolySetUtils::getGeometryAsPolySet(evaluator.evaluateGeometry(*scene->root,false));
  auto set=std::make_shared<InspectionMeshSet>(); set->resultId=stamp.resultId.toStdString(); set->geometryGeneration=17;
  Transform3d transform=Transform3d::Identity();
  transform.scale(Vector3d(-1,1,1)); transform.rotate(Eigen::AngleAxisd(M_PI/6,Vector3d::UnitZ())); transform.scale(Vector3d(2,1,0.5));
  const Vector3d normal(-0.5,-std::sqrt(3.0)/2,0);
  Transform3d rear=Transform3d::Identity(); rear.translate(-14*normal);
  set->pieces={{uint64_t(1)<<40,scene->root->index(),mesh,transform,Color4f(1.0f,0.1f,0.1f,1.0f)},
    {77,scene->root->index(),mesh,rear*transform,Color4f(0.1f,1.0f,0.1f,1.0f)}};
  return set;
}
}

void TestSelection::initTestCase()
{
  // STARTUPINFO's hidden launch can leave the HWND hidden while QWidget reports visible.
  // Force a second native show before tests that require real paint/input delivery.
  window->hide(); window->show(); window->raise(); window->activateWindow();
  QVERIFY(QTest::qWaitForWindowExposed(window));
  QTRY_VERIFY(window->qglview->isValid());
  window->qglview->makeCurrent();
  const auto info = QByteArray::fromStdString(window->qglview->getRendererInfo());
  window->qglview->doneCurrent();
  const auto output = qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");
  if (!output.isEmpty()) {
    QFile file(output+"/renderer-info.txt");
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(info), qint64(info.size()));
  }
}

void TestSelection::init()
{
  // Earlier GUI classes deliberately retain fixture tabs. Clear their dirty flags
  // before closing them so this class never prompts or writes those fixtures.
  for (auto editor : window->tabManager->editorList) editor->setContentModified(false);
  QTimer discard;
  discardFixtureDialogs(discard);
  restoreWindowInitialState();
  window->show(); window->raise(); window->activateWindow();
  QTRY_VERIFY(window->qglview->isVisible());
  window->designActionAutoReload->setChecked(false);
  previousBackend = static_cast<int>(RenderSettings::inst()->backend3D);
  window->qglview->navigation()->settings.animate = false;
  window->viewActionThrownTogether->setChecked(false);
  window->viewActionPreview->setChecked(true);
}
void TestSelection::cleanup()
{
  RenderSettings::inst()->backend3D = static_cast<RenderBackend3D>(previousBackend);
  for (auto editor : window->tabManager->editorList) editor->setContentModified(false);
  QTimer discard;
  discardFixtureDialogs(discard);
  restoreWindowInitialState();
  window->designActionAutoReload->setChecked(false);
}

void TestSelection::visibleSelectionFeedback()
{
  window->activeEditor->setPlainText("for(x=[-8,8]) translate([x,0,0]) cube(10,center=true);");
  QSignalSpy completed(window,&MainWindow::compilationDone);
  window->actionRenderPreview();QTRY_VERIFY_WITH_TIMEOUT(!completed.empty(),15000);
  auto view=window->qglview;
  window->inspectorDock->show();window->inspectorDock->raise();
  QTest::qWait(100);
  // Keep projection stable when Inspector's property text changes its size hint.
  const auto oldMinimum=view->minimumSize(),oldMaximum=view->maximumSize();
  view->setFixedSize(view->size());
  const auto oldScheme=view->colorscheme;
  struct Restore {QGLView *view;QSize minimum,maximum;const ColorScheme *scheme;
    ~Restore(){view->setMinimumSize(minimum);view->setMaximumSize(maximum);view->setColorScheme(*scheme);}
  } restore{view,oldMinimum,oldMaximum,oldScheme};
  view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d::Zero(),65,45,Camera::ProjectionType::ORTHOGONAL});
  for (bool light : {false,true}) {
    ColorScheme scheme=*oldScheme;
    scheme[RenderColor::BACKGROUND_COLOR]=light?Color4f(.95f,.95f,.95f,1.f):Color4f(.06f,.06f,.06f,1.f);
    view->setColorScheme(scheme);
    window->selectionService->clear();view->repaint();
    const auto empty=view->grabFramebuffer();
    const auto model=view->grabFrame(QGLView::CaptureContent::ModelOnly);
    for(double x : {-8.,8.}) {
      const auto pick=window->selectionService->pick(projectedPixel(*view,{x,-5,0}),window->evaluationBinding->stamp());
      QVERIFY2(pick.hit,qPrintable(pick.detail));
      const auto& state=window->selectionService->state();QVERIFY(state.bounds);
      QVERIFY((state.bounds->center()-Vector3d(x,0,0)).norm()<1e-6);
      view->repaint();const auto selected=view->grabFramebuffer();
      QVERIFY(selected!=empty);
      QCOMPARE(view->grabFrame(QGLView::CaptureContent::ModelOnly),model);
      if(x==8 && !qEnvironmentVariableIsEmpty("OPENSCAD_GUI_TEST_OUTPUT"))
        QVERIFY(selected.save(qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT")+(light?"/selection-light.png":"/selection-dark.png")));
    }
    window->selectionService->clear();view->repaint();
    QCOMPARE(view->grabFramebuffer(),empty);
    view->setColorScheme(*oldScheme);
  }
  window->inspectorDock->beginSelection();
  QVERIFY(window->viewportTools->active());
  QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projectedPixel(*view,{8,-5,0}).toPoint());
  QVERIFY(!window->viewportTools->active());
  QVERIFY(window->selectionService->state().surface);
  window->activeEditor->setPlainText("cube(1);");
  QVERIFY(!window->selectionService->state().bounds);
}

void TestSelection::surfacePreview()
{
  for (const auto& fixture : surfaceFixtures) {
    assertSurface(window,fixture,false);
    if (QTest::currentTestFailed()) return;
  }
  assertSurface(window,{"highlight and background", "#cube(10,center=true); %translate([0,-12,0]) cube(8,center=true);", {0,-5,0},{0,-1,0}},false);
}
void TestSelection::surfaceFinal()
{
  for (const auto backend : {RenderBackend3D::ManifoldBackend, RenderBackend3D::CGALBackend}) {
    RenderSettings::inst()->backend3D = backend;
    for (const auto& fixture : surfaceFixtures) {
      assertSurface(window,fixture,true);
      if (QTest::currentTestFailed()) return;
    }
  }
}
void TestSelection::surfaceCapabilities()
{
  window->activeEditor->setPlainText("square(10,center=true);");
  window->actionRenderPreview();
  auto binding=window->evaluationBinding;
  QTRY_VERIFY(binding->current());
  QCOMPARE(window->selectionService->pick({10,10},binding->stamp()).error, Selection::PickError::Unsupported);
  window->activeEditor->setPlainText("cube(10,center=true);");
  window->actionRenderPreview();
  QTRY_VERIFY(binding->current());
  window->viewActionThrownTogether->trigger();
  QCOMPARE(window->selectionService->pick({10,10},binding->stamp()).error, Selection::PickError::Unsupported);
  window->activeEditor->setPlainText("");
  window->actionRenderPreview();
  QTRY_VERIFY(binding->current());
  QCOMPARE(window->selectionService->pick({10,10},binding->stamp()).error, Selection::PickError::Miss);
}
void TestSelection::recenteredSurface()
{
  window->activeEditor->setPlainText("cube(10,center=true);");
  window->actionRenderPreview();
  QTRY_VERIFY(window->evaluationBinding->current());
  const auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());
  Tree tree(scene->root,scene->documentPath);
  GeometryEvaluator evaluator(tree);
  auto geometry=evaluator.evaluateGeometry(*scene->root,false);
  QVERIFY(geometry != nullptr);
  auto view=window->qglview;
  const Vector3d origin(10000,-20000,30000);
  view->navigation()->apply({Vector3d::Zero(), -origin, 50,45,Camera::ProjectionType::ORTHOGONAL});
  const auto logical=projectedPixel(*view,origin);
  const auto pixel=SurfacePickMath::pixel(logical.x(),logical.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height);
  QVERIFY(pixel);
  view->makeCurrent();
  SurfacePick::Result result;
  {
    OriginRenderer renderer(geometry,origin);
    Renderer::SurfaceContext context;
    result=SurfacePick::prepareAndRead(*view,renderer,context,pixel->x(),pixel->y(),view->cam.pixel_width,view->cam.pixel_height);
  }
  view->doneCurrent();
  QCOMPARE(result.error,SurfacePick::Error::None);
  qInfo("Recentered point %.12g %.12g %.12g, normal %.9g %.9g %.9g", result.point.x(),result.point.y(),result.point.z(),result.normal.x(),result.normal.y(),result.normal.z());
  const Eigen::Map<const Eigen::Matrix4d> projection(view->projection), modelview(view->modelview);
  const Eigen::Matrix4d renderModelview = modelview*Transform3d(Eigen::Translation3d(origin)).matrix();
  Eigen::Vector4d clip = projection*modelview*Eigen::Vector4d(result.point.x(),result.point.y(),result.point.z(),1);
  const double depth = (clip.z()/clip.w()+1)/2;
  const auto quantized = SurfacePickMath::unproject(pixel->x(),pixel->y(),depth+2.0/16777215,
    view->cam.pixel_width,view->cam.pixel_height,projection,renderModelview,origin);
  QVERIFY(quantized);
  const double tolerance = std::max(1e-5*geometry->getBoundingBox().diagonal().norm(), (*quantized-result.point).norm());
  QVERIFY(std::abs(result.point.y()-(origin.y()-5)) <= tolerance);
  QVERIFY(result.normal.dot(Vector3d(0,-1,0)) > 0.999999);
}

void TestSelection::dirtyDependencies()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QVERIFY(QDir(directory.path()).mkdir("nested"));
  const auto root = directory.filePath("root.scad");
  const auto values = directory.filePath("nested/values.scad");
  const auto nested = directory.filePath("nested/more.scad");
  const auto library = directory.filePath("helper.scad");
  QVERIFY(writeSource(root, "include <nested/values.scad>\nuse <helper.scad>\ncube([height,size_value(),depth]);\n"));
  QVERIFY(writeSource(values, "include <more.scad>\nheight=1;\n"));
  QVERIFY(writeSource(nested, "depth=13;\r\n"));
  QVERIFY(writeSource(library, "function size_value()=2;\n"));
  window->tabManager->open(values);
  auto valuesEditor = window->activeEditor;
  valuesEditor->setPlainText(QString::fromUtf8("// \xc3\xa9\r\ninclude <more.scad>\r\nheight=7;\r\n"));
  window->tabManager->open(library);
  auto libraryEditor = window->activeEditor;
  libraryEditor->setPlainText("function size_value()=11;\n");
  window->tabManager->open(root);
  auto rootEditor = window->activeEditor;
  QSignalSpy completed(window, &MainWindow::compilationDone);
  window->actionRenderPreview();
  QTRY_VERIFY(!completed.empty());
  auto binding = window->evaluationBinding;
  auto first = binding->stamp();
  auto scene = binding->scene(first);
  QVERIFY2(scene != nullptr, qPrintable(binding->staleReason()));
  QCOMPARE(scene->inputs->consumed().size(), size_t(4));
  const auto bounds = evaluatedBounds(scene);
  QCOMPARE(bounds.max().x(), 7.0);
  QCOMPARE(bounds.max().y(), 11.0);
  QCOMPARE(bounds.max().z(), 13.0);
  QCOMPARE(first.resultId, window->annotationController->status().value("displayed_result_id").toString());
  libraryEditor->setPlainText("function size_value()=17;\n");
  QVERIFY(!binding->current());
  QVERIFY(!binding->scene(first));
  QCOMPARE(window->activeEditor, rootEditor);
  const auto previousCount = completed.count();
  window->actionRenderPreview();
  QTRY_VERIFY(completed.count() > previousCount);
  auto next = binding->scene(binding->stamp());
  QVERIFY(next != nullptr);
  QCOMPARE(evaluatedBounds(next).max().y(), 17.0);
  QVERIFY(!binding->validate(first));
}

void TestSelection::reloadIgnoresUnsavedEdits()
{
  QTemporaryDir directory;
  const auto root = directory.filePath("root.scad");
  QVERIFY(writeSource(root, "cube(1);\n"));
  window->tabManager->open(root);
  window->actionRenderPreview();
  auto binding = window->evaluationBinding;
  QTRY_VERIFY(binding->current());
  const auto installed = binding->stamp().resultId;
  window->activeEditor->setPlainText("cube(2);\n");
  QVERIFY(!binding->current());
  // Exercise the timer's entry point without leaving a repeating timer running.
  window->checkAutoReload();
  QTest::qWait(400);
  QCOMPARE(binding->stamp().resultId, installed);
  QVERIFY(!binding->current());
  window->actionRenderPreview();
  QTRY_VERIFY(binding->current());
  QVERIFY(binding->stamp().resultId != installed);
  QCOMPARE(evaluatedBounds(binding->scene(binding->stamp())).max().x(), 2.0);
}

void TestSelection::cachedDisplayFreshness()
{
  QTemporaryDir directory;
  const auto root = directory.filePath("root.scad");
  const auto dependency = directory.filePath("size.scad");
  QVERIFY(writeSource(root, "include <size.scad>\ncube(size);\n"));
  QVERIFY(writeSource(dependency, "size=1;\n"));
  window->tabManager->open(root);
  window->actionRenderPreview();
  auto binding = window->evaluationBinding;
  QTRY_VERIFY(binding->current());
  const auto stamp = binding->stamp();
  QVERIFY(binding->validate(stamp));
  QVERIFY(writeSource(dependency, "size=2;\n"));
  // No event delivery yet: display checks must not read the changed file.
  for (int i = 0; i < 1000; ++i) QVERIFY(binding->current());
  // Action validation must reject changed bytes even before the watcher fires.
  QVERIFY(!binding->validate(stamp));
  QTRY_VERIFY(!binding->current());
  window->checkAutoReload();
  QTRY_VERIFY(binding->current());
  QCOMPARE(evaluatedBounds(binding->scene(binding->stamp())).max().x(), 2.0);
  window->tabManager->open(dependency);
  QCoreApplication::processEvents();
  window->activeEditor->setPlainText("size=3;\n");
  QVERIFY(!binding->current());
  QVERIFY(window->tabManager->save(window->activeEditor));
  window->tabManager->open(root);
  QTRY_VERIFY(([&] { window->checkAutoReload(); return binding->current(); })());
  QCOMPARE(evaluatedBounds(binding->scene(binding->stamp())).max().x(), 3.0);
}

void TestSelection::conflictingBuffers()
{
  QTemporaryDir directory;
  const auto path = directory.filePath("same.scad");
  QVERIFY(writeSource(path, "cube(1);\n"));
  window->tabManager->open(path);
  auto editor = window->activeEditor;
  editor->setPlainText("cube(7);\n");
  auto second = std::make_unique<MainWindow>(QStringList{path});
  second->designActionAutoReload->setChecked(false);
  second->activeEditor->setPlainText("cube(8);\n");
  window->actionRenderPreview();
  QTRY_COMPARE(window->annotationController->status().value("evaluation").toObject().value("outcome").toString(), QString("failed"));
  QVERIFY(!window->evaluationBinding->current());
  second->activeEditor->setPlainText("cube(7);\n");
  window->actionRenderPreview();
  QTRY_VERIFY(window->evaluationBinding->current());
  auto scene = window->evaluationBinding->scene(window->evaluationBinding->stamp());
  QVERIFY(scene != nullptr);
  QCOMPARE(scene->inputs->consumed().size(), size_t(1));
  QCOMPARE(evaluatedBounds(scene).max().x(), 7.0);
}

void TestSelection::staleAndWrongDocument()
{
  window->tabManager->actionNew();
  auto editor = window->activeEditor;
  editor->setPlainText("cube(9);\n");
  QSignalSpy completed(window, &MainWindow::compilationDone);
  window->actionRenderPreview();
  QTRY_VERIFY(!completed.empty());
  auto binding = window->evaluationBinding;
  auto stamp = binding->stamp();
  QVERIFY(binding->scene(stamp) != nullptr);
  const auto renderer = window->qglview->getRenderer();
  editor->setPlainText("cube( ;");
  window->actionRenderPreview();
  QTRY_COMPARE(window->annotationController->status().value("evaluation").toObject().value("outcome").toString(), QString("failed"));
  QCOMPARE(window->qglview->getRenderer(), renderer);
  QVERIFY(!binding->scene(stamp));
  editor->setPlainText("cube(9);\n");
  const auto retryCount = completed.count();
  window->actionRenderPreview();
  QTRY_VERIFY(completed.count() > retryCount);
  stamp = binding->stamp();
  QVERIFY(binding->validate(stamp));
  window->tabManager->actionNew();
  QVERIFY(window->activeEditor != editor);
  QVERIFY(!binding->validate(stamp));
}

void TestSelection::retainedSceneAfterClose()
{
  window->tabManager->actionNew();
  auto editor = window->activeEditor;
  editor->setPlainText("module old_part() { cube([19,23,29]); } old_part();\n");
  QSignalSpy completed(window, &MainWindow::compilationDone);
  window->actionRenderPreview();
  QTRY_VERIFY(!completed.empty());
  auto binding = window->evaluationBinding;
  const auto stamp = binding->stamp();
  auto scene = binding->scene(stamp);
  QVERIFY(scene != nullptr);
  std::promise<void> release;
  auto gate = release.get_future().share();
  auto result = std::async(std::launch::async, [scene, gate] {
    gate.wait();
    Tree workerTree(scene->root, scene->documentPath);
    return workerTree.getString(*scene->root, "");
  });
  editor->setContentModified(false);
  window->tabManager->closeCurrentTab();
  window->activeEditor->setPlainText("cube(3);\n");
  window->actionRenderPreview();
  release.set_value(); // Release even if a subsequent assertion fails.
  QVERIFY(!binding->validate(stamp));
  QVERIFY(result.get().find("cube") != std::string::npos);
  QCOMPARE(evaluatedBounds(scene).max().x(), 19.0);
}

void TestSelection::inspectionColors()
{
  window->activeEditor->setPlainText("cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone);window->actionRenderPreview();QTRY_VERIFY(!compiled.empty());
  auto view=window->qglview;auto coordinator=window->inspectionPresentation;
  const auto originalScheme=view->colorscheme;
  ColorScheme blue=*originalScheme,green=*originalScheme;
  blue[RenderColor::OPENCSG_FACE_FRONT_COLOR]=Color4f(.1f,.2f,.9f,1.f);
  green[RenderColor::OPENCSG_FACE_FRONT_COLOR]=Color4f(.1f,.9f,.2f,1.f);
  struct Restore {QGLView *view;const ColorScheme *scheme;~Restore(){view->setColorScheme(*scheme);}} restore{view,originalScheme};
  view->setColorScheme(blue);
  const auto stamp=coordinator->stamp();auto meshes=inspectionFixture(window,stamp);
  meshes->pieces[0].localToWorld=Transform3d(Eigen::Translation3d(-8,0,0));meshes->pieces[0].color=Color4f();
  meshes->pieces[1].localToWorld=Transform3d(Eigen::Translation3d(8,0,0));meshes->pieces[1].color=Color4f(.9f,.1f,.1f,1.f);
  QObject lifetime;int starts=0;
  auto provider=coordinator->registerMeshProvider("theme-fixture",0,&lifetime,[&](auto,auto,auto done){
    ++starts;done({InspectionPresentationController::Status::Ready,meshes,{}});return InspectionPresentationController::Cancel{};
  });
  auto request=coordinator->requestMeshes("theme-fixture",&lifetime);
  QTRY_COMPARE(coordinator->status(),InspectionPresentationController::Status::Ready);
  view->navigation()->apply({CameraPose::upright(Vector3d(0,-1,0)),Vector3d::Zero(),65,45,Camera::ProjectionType::ORTHOGONAL});
  QTest::qWait(30);
  auto sample=[&](const QImage& image,double x){return image.pixelColor((projectedPixel(*view,{x,-5,0})*view->devicePixelRatio()).toPoint());};
  const auto initial=view->grabFrame(QGLView::CaptureContent::ModelOnly);
  const auto initialStarts=starts;
  QVERIFY(sample(initial,-8).blue()>sample(initial,-8).green());
  const auto explicitColor=sample(initial,8);QVERIFY(explicitColor.red()>explicitColor.blue());
  // Theme changes must update existing VBO colors without rerunning the geometry provider.
  view->setColorScheme(green);
  const auto changed=view->grabFrame(QGLView::CaptureContent::ModelOnly);
  QVERIFY(sample(changed,-8).green()>sample(changed,-8).blue());
  QCOMPARE(sample(changed,8),explicitColor);QCOMPARE(starts,initialStarts);
  view->setColorScheme(green);QCOMPARE(view->grabFrame(QGLView::CaptureContent::ModelOnly),changed);
  request.reset();view->setColorScheme(*originalScheme);
}

void TestSelection::inspectionMeshes()
{
  window->activeEditor->setPlainText("cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->actionRenderPreview(); QTRY_VERIFY(!compiled.empty());
  auto coordinator=window->inspectionPresentation; auto view=window->qglview;
  QTest::qWait(1);
  const auto original=view->renderer;
  const auto originalStamp=coordinator->stamp();
  auto set=inspectionFixture(window,originalStamp);
  QObject lifetime;
  int starts=0;
  auto provider=coordinator->registerMeshProvider("fixture",0,&lifetime,[&](auto scene,auto stamp,auto done) {
    ++starts; if (!scene) throw std::runtime_error("Fixture scene unavailable");
    done({InspectionPresentationController::Status::Ready,set,{}});
    return InspectionPresentationController::Cancel{};
  });
  QVERIFY(provider); QCOMPARE(view->renderer,original);
  auto request=coordinator->requestMeshes("fixture display",&lifetime);
  QTRY_COMPARE(coordinator->status(),InspectionPresentationController::Status::Ready);
  QCOMPARE(starts,1); QCOMPARE(coordinator->stamp().resultId,originalStamp.resultId);
  QVERIFY(coordinator->stamp().viewportGeneration>originalStamp.viewportGeneration);
  QCOMPARE(window->annotationController->status().value("displayed_result_id").toString(),originalStamp.resultId);
  auto secondRequest=coordinator->requestMeshes("another consumer",&lifetime); QCOMPARE(starts,1);
  const Vector3d normal(-0.5,-std::sqrt(3.0)/2,0),point(-2.5,-5*std::sqrt(3.0)/2,0);
  view->navigation()->apply({CameraPose::upright(normal),-point,65,45,Camera::ProjectionType::ORTHOGONAL});
  view->update(); QTest::qWait(20);
  auto pixel=projectedPixel(*view,point);
  auto pick=window->selectionService->pick(pixel,coordinator->stamp());
  QVERIFY2(pick.hit.has_value(),qPrintable(pick.detail)); QCOMPARE(pick.hit->object.drawId,uint64_t(1)<<40);
  QVERIFY(pick.hit->outwardNormal.dot(normal)>std::cos(0.1*M_PI/180));
  QVERIFY(pick.hit->localPoint && pick.hit->localToWorld);
  const double tolerance=1e-5*view->renderer->getBoundingBox().diagonal().norm();
  QVERIFY(std::abs(normal.dot(pick.hit->worldPoint-point))<=tolerance);
  QVERIFY(std::abs(pick.hit->localPoint->y()+5)<=tolerance);
  QVERIFY(((*pick.hit->localToWorld)*(*pick.hit->localPoint)-pick.hit->worldPoint).norm()<1e-7);
  const auto generation=coordinator->stamp().viewportGeneration;
  const auto both=view->grabFrame(QGLView::CaptureContent::ModelOnly);
  QVERIFY(request.setMask({1,{77}},coordinator->stamp(),17));
  QCOMPARE(coordinator->stamp().viewportGeneration,generation); QVERIFY(!window->selectionService->state().object);
  pick=window->selectionService->pick(pixel,coordinator->stamp()); QVERIFY(pick.hit); QCOMPARE(pick.hit->object.drawId,uint64_t(77));
  QVERIFY(std::abs(normal.dot(pick.hit->worldPoint-point+14*normal))<=tolerance);
  QVERIFY(pick.hit->outwardNormal.dot(normal)>std::cos(0.1*M_PI/180));
  const auto rear=view->grabFrame(QGLView::CaptureContent::ModelOnly); QVERIFY(rear!=both);
  QVERIFY(secondRequest.setMask({1,{}},coordinator->stamp(),17));
  QCOMPARE(window->selectionService->pick(pixel,coordinator->stamp()).error,Selection::PickError::Miss);
  secondRequest.clearMask(); request.clearMask();
  QCOMPARE(view->grabFrame(QGLView::CaptureContent::ModelOnly),both);
  auto probe=std::make_shared<EffectProbe>();
  auto effect=coordinator->registerMeshEffect("predicate",0,&lifetime,[probe](auto child) { return std::make_shared<ProbeEffect>(child,probe); });
  QVERIFY(effect); QVERIFY(window->selectionService->pick(pixel,coordinator->stamp()).hit);
  view->grabFrame(QGLView::CaptureContent::ModelOnly); QVERIFY(probe->colors>0 && probe->picks>=2);
  const auto effectGeneration=coordinator->stamp().viewportGeneration;
  probe->blocked=true; effect.uniformsChanged();
  QCOMPARE(coordinator->stamp().viewportGeneration,effectGeneration);
  QCOMPARE(window->selectionService->pick(pixel,coordinator->stamp()).error,Selection::PickError::Miss);
  QVERIFY(view->grabFrame(QGLView::CaptureContent::ModelOnly)!=both);
  probe->blocked=false; effect.uniformsChanged(); QVERIFY(window->selectionService->pick(pixel,coordinator->stamp()).hit);
  effect.reset(); secondRequest.reset(); QVERIFY(view->renderer!=original);
  request.reset(); QCOMPARE(view->renderer,original); QVERIFY(coordinator->stamp().viewportGeneration>effectGeneration);
  QVERIFY(window->evaluationBinding->current()); QCOMPARE(starts,1);
  // A valid empty display has no default renderer, and must restore that exact state.
  QVERIFY(window->evaluationBinding->replacePresentationRenderer(nullptr,coordinator->stamp()));
  QVERIFY(window->evaluationBinding->current()); QVERIFY(!view->renderer);
  provider.reset();
  auto emptyProvider=coordinator->registerMeshProvider("empty",0,&lifetime,[](auto,auto,auto done) {
    done({InspectionPresentationController::Status::Empty}); return InspectionPresentationController::Cancel{};
  });
  auto emptyRequest=coordinator->requestMeshes("empty display",&lifetime);
  QTRY_COMPARE(coordinator->status(),InspectionPresentationController::Status::Empty);
  QVERIFY(view->renderer); emptyRequest.reset(); QVERIFY(!view->renderer); QVERIFY(window->evaluationBinding->current());
  QCOMPARE(window->annotationController->status().value("displayed_result_id").toString(),originalStamp.resultId);
}

void TestSelection::inspectionProviderLifetime()
{
  window->activeEditor->setPlainText("cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->actionRenderPreview(); QTRY_VERIFY(!compiled.empty()); QTest::qWait(1);
  auto coordinator=window->inspectionPresentation;
  auto set=inspectionFixture(window,coordinator->stamp());
  auto lifetime=std::make_unique<QObject>();
  InspectionPresentationController::Done rootDone,highDone;
  int starts=0,cancelled=0;
  auto root=coordinator->registerMeshProvider("root fixture",0,lifetime.get(),[&](auto,auto,auto done) {
    ++starts; rootDone=done; return InspectionPresentationController::Cancel([&] { ++cancelled; });
  });
  auto request=coordinator->requestMeshes("fixture request",lifetime.get());
  QCOMPARE(coordinator->status(),InspectionPresentationController::Status::Preparing); QCOMPARE(starts,1);
  const auto stale=rootDone;
  auto high=coordinator->registerMeshProvider("high fixture",200,lifetime.get(),[&](auto,auto,auto done) {
    highDone=done; return InspectionPresentationController::Cancel([&] { ++cancelled; });
  });
  QCOMPARE(cancelled,1);
  std::async(std::launch::async,[stale,set] { stale({InspectionPresentationController::Status::Ready,set,{}}); }).get();
  QTest::qWait(10); QCOMPARE(coordinator->status(),InspectionPresentationController::Status::Preparing);
  highDone({InspectionPresentationController::Status::Failed,{},"provider failed"});
  QTRY_COMPARE(coordinator->status(),InspectionPresentationController::Status::Failed); QCOMPARE(starts,1);
  QVERIFY(high.setEnabled(false)); QCOMPARE(starts,2);
  rootDone({InspectionPresentationController::Status::Ready,set,{}}); QTRY_COMPARE(coordinator->status(),InspectionPresentationController::Status::Ready);
  QVERIFY(high.setEnabled(true)); QCOMPARE(coordinator->status(),InspectionPresentationController::Status::Preparing);
  const auto afterClose=highDone;
  lifetime.reset(); QCOMPARE(coordinator->status(),InspectionPresentationController::Status::Inactive);
  QVERIFY(!root && !high && !request);
  afterClose({InspectionPresentationController::Status::Ready,set,{}}); QTest::qWait(10);
  QCOMPARE(coordinator->status(),InspectionPresentationController::Status::Inactive);
}

void TestSelection::toolLifecycle()
{
  window->activeEditor->setPlainText("cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->actionRenderPreview(); QTRY_VERIFY(!compiled.empty());
  auto view=window->qglview; auto tools=window->viewportTools; auto binding=window->evaluationBinding;
  view->navigation()->apply({CameraPose::upright({0,-1,0}),{0,5,0},50,45,Camera::ProjectionType::ORTHOGONAL});
  view->update(); QTest::qWait(20);
  QObject lifetime; int deliveries=0,cancelled=0;
  auto callback=[&](Selection::PickResult result) { ++deliveries; if (result.error==Selection::PickError::Cancelled) ++cancelled; };
  auto token=tools->beginPick({"native fixture",binding->stamp(),true},&lifetime,callback); QVERIFY(token);
  QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,{1,1}); QCOMPARE(tools->active(),token); QCOMPARE(deliveries,0);
  const auto before=view->navigation()->current();
  QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,view->rect().center());
  QMouseEvent motion(QEvent::MouseMove,QPointF(view->rect().center()+QPoint(40,25)),QPointF(view->mapToGlobal(view->rect().center()+QPoint(40,25))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
  QApplication::sendEvent(view,&motion);
  QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,view->rect().center()+QPoint(40,25));
  QCOMPARE(tools->active(),token); QCOMPARE(deliveries,0); QVERIFY(!(view->navigation()->current()==before));
  QTest::keyClick(view,Qt::Key_Escape); QVERIFY(!tools->active()); QCOMPARE(deliveries,1); QCOMPARE(cancelled,1);
  tools->cancel(token,Selection::CancelReason::Escape); QCOMPARE(deliveries,1);
  view->navigation()->apply(before); QTest::qWait(20);
  token=tools->beginPick({"native fixture",binding->stamp(),true},&lifetime,callback);
  const auto point=projectedPixel(*view,{0,-5,0}).toPoint(); QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,point);
  QVERIFY(!tools->active()); QCOMPARE(deliveries,2); QCOMPARE(cancelled,1); QVERIFY(window->selectionService->state().surface);
  token=tools->beginPick({"native fixture",binding->stamp(),true},&lifetime,callback);
  auto doomed=std::make_unique<QObject>();
  auto replacement=tools->beginPick({"replacement",binding->stamp(),false},doomed.get(),callback);
  QCOMPARE(cancelled,2); QVERIFY(replacement && replacement!=token);
  doomed.reset(); QCOMPARE(cancelled,3); QVERIFY(!tools->active());
  token=tools->beginPick({"native fixture",binding->stamp(),true},&lifetime,callback);
  window->annotationController->startDrawing(); QVERIFY(window->annotationController->drafting()); QCOMPARE(cancelled,4);
  const auto annotation=tools->active(); QVERIFY(annotation && annotation!=token);
  token=tools->beginPick({"native fixture",binding->stamp(),true},&lifetime,callback);
  QVERIFY(token); QVERIFY(!window->annotationController->drafting()); QVERIFY(!GuiLocker::isLocked());
  window->activeEditor->setPlainText("cube(11,center=true);"); QCOMPARE(cancelled,5); QVERIFY(!tools->active());
  window->actionRenderPreview(); QTRY_VERIFY(binding->current());
  token=tools->beginPick({"native fixture",binding->stamp(),true},&lifetime,callback);
  window->tabManager->actionNew(); QVERIFY(!tools->active()); QCOMPARE(cancelled,6);
  QVERIFY(!tools->beginPick({"stale",binding->stamp(),true},&lifetime,callback)); QCOMPARE(deliveries,8);
}

void TestSelection::inspectorExtensions()
{
  window->activeEditor->setPlainText("cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->actionRenderPreview(); QTRY_VERIFY(!compiled.empty());
  auto view=window->qglview;
  view->navigation()->apply({CameraPose::upright({0,-1,0}),{0,5,0},50,45,Camera::ProjectionType::ORTHOGONAL});
  window->inspectorDock->beginSelection(); QTest::qWait(20);
  QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,projectedPixel(*view,{0,-5,0}).toPoint());
  QVERIFY(window->selectionService->state().surface);
  auto text=window->inspectorDock->findChild<QLabel*>("geometricProperties"); QVERIFY(text);
  QVERIFY(text->text().contains("World point: [")); QVERIFY(text->text().contains("Outward normal: ["));
  const auto output=qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");
  if (!output.isEmpty()) { QTest::qWait(20); QVERIFY(window->grab().save(output+"/inspector.png")); }
  auto owner=std::make_unique<QObject>(); int builds=0;
  QPointer<QWidget> old;
  auto token=window->inspectorDock->registerContribution({"fixture",10,owner.get(),[&](const auto& state,const auto& stamp) {
    ++builds; auto label=new QLabel(state.object ? "Fixture selected" : "Fixture cleared"); label->setObjectName("fixtureContribution"); old=label; return label;
  }});
  QVERIFY(token); QVERIFY(old); QCOMPARE(builds,1);
  QVERIFY(!window->inspectorDock->registerContribution({"fixture",20,owner.get(),[](const auto&,const auto&) { return new QLabel; }}));
  const auto previous=old; window->selectionService->clear(); QVERIFY(previous.isNull()); QCOMPARE(builds,2);
  owner.reset(); QVERIFY(!token); QVERIFY(old.isNull()); window->selectionService->clear(); QCOMPARE(builds,2);
  window->inspectorDock->hide();
}

void TestSelection::overlayIsolation()
{
  window->activeEditor->setPlainText("cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->actionRenderPreview(); QTRY_VERIFY(!compiled.empty());
  auto view=window->qglview; auto overlays=window->viewportOverlays;
  view->navigation()->apply({CameraPose::upright({0,-1,0}),{0,5,0},50,45,Camera::ProjectionType::ORTHOGONAL});
  QTest::qWait(20);
  const auto clean=view->grabFrame(QGLView::CaptureContent::ModelOnly);
  auto owner=std::make_unique<QObject>(); std::vector<int> order;
  auto a=overlays->registerOverlay(owner.get(),Selection::OverlayLayer::WorldOverlay,0,[&](const auto& context) {
    order.push_back(2); QVERIFY(context.stamp.resultId==window->evaluationBinding->stamp().resultId);
    glViewport(0,0,1,1); glDepthMask(GL_FALSE); glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE); glDisable(GL_DEPTH_TEST);
  });
  auto b=overlays->registerOverlay(owner.get(),Selection::OverlayLayer::WorldDepth,0,[&](const auto&) { order.push_back(1); });
  auto c=overlays->registerOverlay(owner.get(),Selection::OverlayLayer::HUD,0,[&](const auto&) {
    order.push_back(3); GLint viewport[4]; GLboolean mask[4]; glGetIntegerv(GL_VIEWPORT,viewport); glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    QCOMPARE(viewport[2],view->cam.pixel_width); QVERIFY(mask[0] && mask[1] && mask[2] && mask[3]);
  });
  view->update(); QTRY_VERIFY(order.size()>=3); QCOMPARE(order[0],1); QCOMPARE(order[1],2); QCOMPARE(order[2],3);
  const auto drawn=order.size(); const auto pick=window->selectionService->pick(projectedPixel(*view,{0,-5,0}),window->evaluationBinding->stamp());
  QVERIFY(pick.hit); QCOMPARE(order.size(),drawn);
  QCOMPARE(view->grabFrame(QGLView::CaptureContent::ModelOnly),clean);
  owner.reset(); QVERIFY(!a && !b && !c); order.clear(); view->update(); QTest::qWait(20); QVERIFY(order.empty());
}

void TestSelection::dragAndMeasurementOwnership()
{
  RenderSettings::inst()->backend3D=RenderBackend3D::ManifoldBackend;
  window->activeEditor->setPlainText("cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->designActionRender->trigger(); QTRY_VERIFY(!compiled.empty());
  auto view=window->qglview; auto tools=window->viewportTools; auto binding=window->evaluationBinding;
  QObject lifetime; int moves=0,releases=0,cancels=0;
  Selection::DragCallbacks callbacks{[&](auto,auto) { ++moves; },[&](auto,auto) { ++releases; },[&](auto) { ++cancels; }};
  auto token=tools->beginDrag({"fixture drag",binding->stamp(),true},&lifetime,{10,10},callbacks); QVERIFY(token);
  QMouseEvent move(QEvent::MouseMove,{20,25},{20,25},Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
  QVERIFY(tools->route(move)); QCOMPARE(moves,1);
  QMouseEvent wrong(QEvent::MouseButtonRelease,{20,25},{20,25},Qt::RightButton,Qt::NoButton,Qt::NoModifier);
  QVERIFY(!tools->route(wrong)); QCOMPARE(tools->active(),token);
  QMouseEvent release(QEvent::MouseButtonRelease,{20,25},{20,25},Qt::LeftButton,Qt::NoButton,Qt::ShiftModifier);
  QVERIFY(tools->route(release)); QCOMPARE(releases,1); QCOMPARE(cancels,0); QVERIFY(!tools->active());
  tools->cancel(token,Selection::CancelReason::Escape); QCOMPARE(cancels,0);
  token=tools->beginDrag({"fixture drag",binding->stamp(),true},&lifetime,{10,10},callbacks);
  tools->cancel(token,Selection::CancelReason::Escape); QCOMPARE(cancels,1); QVERIFY(!tools->route(release)); QCOMPARE(releases,1);
  QVERIFY(window->designActionMeasureDist->isEnabled()); window->designActionMeasureDist->trigger();
  QVERIFY(window->designActionMeasureDist->isChecked()); QVERIFY(tools->active());
  token=tools->beginPick({"replace measurement",binding->stamp(),true},&lifetime,[](auto) {});
  QVERIFY(token); QVERIFY(!window->designActionMeasureDist->isChecked());
  window->designActionMeasureAngle->trigger(); QVERIFY(window->designActionMeasureAngle->isChecked());
  QVERIFY(tools->active()!=token); window->annotationController->startDrawing();
  QVERIFY(window->annotationController->drafting()); QVERIFY(!window->designActionMeasureAngle->isChecked());
  window->annotationController->cancelDrawing(); QVERIFY(!tools->active());
  int cancelled=0;
  token=tools->beginPick({"focus",binding->stamp(),true},&lifetime,[&](auto result) { if (result.error==Selection::PickError::Cancelled) ++cancelled; });
  QVERIFY(token); window->activeEditor->setFocus(); QTRY_VERIFY(!tools->active()); QCOMPARE(cancelled,1);
  auto other=std::make_unique<MainWindow>(QStringList{});
  QVERIFY(!other->viewportTools->beginPick({"wrong window",binding->stamp(),true},other.get(),[](auto) {}));
}

void TestSelection::resizedBordersAndInstances()
{
  window->activeEditor->setPlainText("for(x=[-8,8]) translate([x,0,0]) cube(10,center=true);");
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->actionRenderPreview(); QTRY_VERIFY(!compiled.empty());
  auto view=window->qglview; const auto originalSize=window->size();
  for (const auto& size : {QSize(1040,720),QSize(1260,860)}) {
    window->resize(size); QTest::qWait(30);
    view->navigation()->apply({CameraPose::upright({0,-1,0}),{0,5,0},100,45,Camera::ProjectionType::ORTHOGONAL});
    QTest::qWait(20); const auto stamp=window->evaluationBinding->stamp();
    const auto a=window->selectionService->pick(projectedPixel(*view,{-8,-5,0}),stamp);
    const auto b=window->selectionService->pick(projectedPixel(*view,{8,-5,0}),stamp);
    QVERIFY(a.hit && b.hit); QVERIFY(a.hit->object.drawId!=b.hit->object.drawId);
    QVERIFY(a.hit->worldPoint.x()<0 && b.hit->worldPoint.x()>0);
    for (const auto& point : {QPointF(0,0),QPointF(view->width()-0.5/view->devicePixelRatio(),0),
         QPointF(0,view->height()-0.5/view->devicePixelRatio()),QPointF(view->width()-0.5/view->devicePixelRatio(),view->height()-0.5/view->devicePixelRatio())})
      QCOMPARE(window->selectionService->pick(point,stamp).error,Selection::PickError::Miss);
    QCOMPARE(window->selectionService->pick({double(view->width()),10},stamp).error,Selection::PickError::InvalidViewport);
    QCOMPARE(window->selectionService->pick({10,double(view->height())},stamp).error,Selection::PickError::InvalidViewport);
  }
  window->resize(originalSize);
}

void TestSelection::staleDependencyDuringPick()
{
  QTemporaryDir directory;
  const auto root=directory.filePath("root.scad"),dependency=directory.filePath("piece.scad");
  QVERIFY(writeSource(root,"include <piece.scad>;\n")); QVERIFY(writeSource(dependency,"cube(10,center=true);\n"));
  window->tabManager->open(dependency); auto editor=window->activeEditor;
  editor->setPlainText("cube(11,center=true);\n");
  window->tabManager->open(root); auto rootEditor=window->activeEditor;
  QSignalSpy compiled(window,&MainWindow::compilationDone); window->actionRenderPreview(); QTRY_VERIFY(!compiled.empty());
  auto binding=window->evaluationBinding; auto tools=window->viewportTools;
  const auto stamp=binding->stamp(); auto renderer=window->qglview->renderer;
  QObject lifetime; int cancelled=0,applied=0;
  auto callback=[&](Selection::PickResult result) { if (result.hit) ++applied; if (result.error==Selection::PickError::Cancelled) ++cancelled; };
  QVERIFY(tools->beginPick({"dirty dependency",stamp,true},&lifetime,callback));
  editor->setPlainText("cube(12,center=true);\n"); QCOMPARE(cancelled,1); QCOMPARE(applied,0); QVERIFY(!tools->active());
  QCOMPARE(window->selectionService->pick({20,20},stamp).error,Selection::PickError::Stale);
  window->actionRenderPreview(); QTRY_VERIFY(binding->current());
  auto next=binding->stamp();
  QVERIFY(tools->beginPick({"invalid root",next,true},&lifetime,callback));
  renderer=window->qglview->renderer; rootEditor->setPlainText("cube( ;"); window->actionRenderPreview();
  QTRY_COMPARE(window->annotationController->status().value("evaluation").toObject().value("outcome").toString(),QString("failed"));
  QCOMPARE(window->qglview->renderer,renderer); QCOMPARE(cancelled,2); QCOMPARE(applied,0);
  QVERIFY(!tools->active()); QVERIFY(!binding->validate(next));
}
