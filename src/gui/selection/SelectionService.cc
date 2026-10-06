#include "gui/selection/SelectionService.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/InspectionPresentationController.h"
#include "glview/SurfacePick.h"
#include "core/node.h"
#include <Eigen/LU>
#include <QApplication>

namespace Selection {
SelectionService::SelectionService(MainWindow *window) : QObject(window), window(window)
{
  connect(window->evaluationBinding, &EvaluationBinding::invalidated, this, &SelectionService::clear);
  connect(window->qglview, &QGLView::cameraChanged, this, [this] { ++cameraSerial; });
  connect(window->qglview->navigation(), &CameraNavigation::displayChanged, this, [this] { ++cameraSerial; });
  connect(window->qglview, &QGLView::resized, this, [this] { ++presentationSerial; });
  connect(window->qglview, &QGLView::presentationChanged, this, [this] { ++presentationSerial; clear(); });
  connect(window->qglview, &QGLView::frameAboutToRender, this, [this] { ++presentationSerial; });
}
PickResult SelectionService::pick(QPointF logical, const Stamp& expected)
{
  if (!window) return {PickError::Cancelled, {}, tr("Viewport closed")};
  if (GuiLocker::isLocked() || QApplication::activeModalWidget()) return {PickError::Busy, {}, tr("Evaluation or modal interaction is active")};
  const auto inspection=window->inspectionPresentation;
  if (inspection && inspection->status()!=InspectionPresentationController::Status::Inactive &&
      inspection->status()!=InspectionPresentationController::Status::Ready && inspection->status()!=InspectionPresentationController::Status::Empty)
    return {inspection->status()==InspectionPresentationController::Status::Preparing ? PickError::Busy : PickError::Unsupported,{},inspection->reason()};
  auto binding = window->evaluationBinding;
  QString detail;
  if (!binding->validate(expected, &detail)) return {PickError::Stale, {}, detail};
  auto scene = binding->scene(expected);
  auto view = window->qglview;
  const auto renderer = view->renderer;
  if (!renderer) return {PickError::Miss};
  const auto statistics = renderer->triangleStatistics();
  if (statistics.count && *statistics.count == 0) return {PickError::Miss};
  const auto pixel = SurfacePickMath::pixel(logical.x(), logical.y(), view->devicePixelRatio(),
                                           view->cam.pixel_width, view->cam.pixel_height);
  if (!pixel || !view->isValid()) return {PickError::InvalidViewport, {}, tr("Pixel or GL viewport is invalid")};
  const auto camera = cameraSerial, presentation = presentationSerial;
  const auto pose = view->navigation()->current();
  const auto width = view->cam.pixel_width, height = view->cam.pixel_height;
  view->makeCurrent();
  Renderer::SurfaceContext context;
  const auto result = SurfacePick::prepareAndRead(*view, *renderer, context, pixel->x(), pixel->y(), width, height);
  view->doneCurrent();
  if (!binding->validate(expected) || renderer != view->renderer || cameraSerial != camera || presentationSerial != presentation ||
      !(view->navigation()->current() == pose))
    return {PickError::Stale, {}, tr("Result, camera or viewport presentation changed during picking")};
  switch (result.error) {
    case SurfacePick::Error::Miss: return {PickError::Miss};
    case SurfacePick::Error::Unsupported: return {PickError::Unsupported, {}, QString::fromStdString(result.detail)};
    case SurfacePick::Error::InvalidViewport: return {PickError::InvalidViewport};
    case SurfacePick::Error::BackendFailure: return {PickError::BackendFailure, {}, QString::fromStdString(result.detail)};
    case SurfacePick::Error::None: break;
  }
  const auto found = std::find_if(context.objects.begin(), context.objects.end(),
    [&result](const auto& object) { return object.pickToken == result.drawId; });
  if (found == context.objects.end()) return {PickError::BackendFailure, {}, tr("Unmapped surface draw ID")};
  Hit hit{expected, {expected.resultId, found->drawId, found->nodeIndex}, result.point, result.normal, {}, {}};
  if (!hit.object.nodeIndex) hit.object.nodeIndex = scene->root->index();
  if (found->localToWorld && found->localToWorld->matrix().allFinite() &&
      Eigen::FullPivLU<Eigen::Matrix4d>(found->localToWorld->matrix()).isInvertible()) {
    hit.localToWorld = found->localToWorld;
    hit.localPoint = found->localToWorld->inverse() * hit.worldPoint;
  }
  selected = {hit.object, hit, found->bounds};
  if (selected.bounds) selected.bounds->translate(context.positionOrigin);
  emit selectionChanged(selected);
  return {PickError::None, hit, {}};
}
bool SelectionService::selectObject(const ObjectKey& object, const Stamp& expected)
{
  if (!window || !window->evaluationBinding->validate(expected) || object.resultId != expected.resultId || !object.drawId)
    return false;
  const auto inspection=window->inspectionPresentation;
  if (inspection && inspection->status()!=InspectionPresentationController::Status::Inactive &&
      inspection->status()!=InspectionPresentationController::Status::Ready && inspection->status()!=InspectionPresentationController::Status::Empty) return false;
  auto view = window->qglview;
  if (!view->renderer || !view->isValid()) return false;
  const auto renderer=view->renderer;
  const auto camera=cameraSerial,presentation=presentationSerial;
  view->makeCurrent();
  Renderer::SurfaceContext context;
  SurfacePick::prepareAndRead(*view, *view->renderer, context, -1, -1, view->cam.pixel_width, view->cam.pixel_height);
  view->doneCurrent();
  if (!window->evaluationBinding->validate(expected) || view->renderer!=renderer || cameraSerial!=camera || presentationSerial!=presentation) return false;
  const auto found = std::find_if(context.objects.begin(), context.objects.end(), [&object](const auto& candidate) {
    return candidate.drawId == object.drawId;
  });
  if (!context.supported || found == context.objects.end()) return false;
  const auto scene = window->evaluationBinding->scene(expected);
  if (!scene || object.nodeIndex != (found->nodeIndex ? found->nodeIndex : std::optional<size_t>(scene->root->index()))) return false;
  selected = {object, {}, found->bounds};
  if (selected.bounds) selected.bounds->translate(context.positionOrigin);
  emit selectionChanged(selected);
  return true;
}
void SelectionService::clear()
{
  selected = {};
  emit selectionChanged(selected);
}
}
