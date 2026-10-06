#include "gui/clipping/ClipMeshJob.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/selection/EvaluationBinding.h"
#include "core/Tree.h"
#include "core/TransformNode.h"
#include "core/ColorNode.h"
#include "core/progress.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/Geometry.h"
#include "geometry/PolySetUtils.h"
#ifdef ENABLE_CGAL
#include "geometry/cgal/CGALNefGeometry.h"
#endif
#include "glview/preview/ThrownTogetherRenderer.h"
#include <QElapsedTimer>
#include <QTimer>
#include <thread>
#include <stdexcept>

namespace {
using Status = InspectionPresentationController::Status;
struct Work : std::enable_shared_from_this<Work> {
  QPointer<MainWindow> window;
  std::shared_ptr<QObject> delivery = std::make_shared<QObject>();
  std::shared_ptr<const Selection::EvaluatedScene> scene;
  Selection::Stamp stamp;
  std::shared_ptr<ClipMeshJob::Metrics> counts;
  InspectionPresentationController::Done done;
  std::atomic<bool> cancelled{false};
  std::unique_ptr<GuiLocker> lock;
  std::shared_ptr<const Geometry> finalGeometry;
  QElapsedTimer elapsed;

  void add(const std::shared_ptr<const Geometry>& geometry, InspectionMeshSet& output,
           const Transform3d& transform, const Color4f& color, InspectionPiece::Role role,
           std::optional<size_t> nodeIndex = {}) {
    if (!geometry || geometry->isEmpty()) return;
    if (geometry->getDimension() != 3) throw std::domain_error("Cross section clipping requires three-dimensional mesh geometry");
    if (auto list = std::dynamic_pointer_cast<const GeometryList>(geometry)) {
      for (const auto& [node, child] : list->children) add(child, output, transform, color, role, node ? std::optional<size_t>(node->index()) : nodeIndex);
      return;
    }
    auto conversionGeometry=geometry;
    Transform3d pieceTransform=transform;
#ifdef ENABLE_CGAL
    // The native Nef converter indexes float positions. Recenter before that
    // narrowing step, using a private copy so the evaluated final stays immutable.
    if (dynamic_cast<const CGALNefGeometry *>(geometry.get())) {
      const Vector3d center=geometry->getBoundingBox().center();
      auto local=geometry->copy();local->transform(Transform3d(Eigen::Translation3d(-center)));
      conversionGeometry=std::shared_ptr<const Geometry>(std::move(local));
      pieceTransform=pieceTransform*Transform3d(Eigen::Translation3d(center));
    }
#endif
    auto mesh = PolySetUtils::getGeometryAsPolySet(conversionGeometry);
    if (!mesh || mesh->isEmpty()) throw std::domain_error("This geometry cannot be converted to an inspection mesh");
    if (!mesh->isTriangular()) {
      const Vector3d center=mesh->getBoundingBox().center();
      PolySet local(*mesh);local.transform(Transform3d(Eigen::Translation3d(-center)));
      mesh=PolySetUtils::tessellate_faces(local);
      pieceTransform=pieceTransform*Transform3d(Eigen::Translation3d(center));
    }
    output.pieces.push_back({output.pieces.size()+1, nodeIndex, mesh, pieceTransform, color, role});
  }
  void roles(const std::shared_ptr<const AbstractNode>& node, const Transform3d& parent,
             Color4f color, InspectionMeshSet& output) {
    if (cancelled) throw ProgressCancelException();
    if (node->modinst->isBackground() || node->modinst->isHighlight()) {
      Tree tree(node, scene->documentPath);
      GeometryEvaluator evaluator(tree, true);
      add(evaluator.evaluateGeometry(*node, false), output, parent, color,
          node->modinst->isBackground() ? InspectionPiece::Role::Background : InspectionPiece::Role::Highlight, node->index());
      return;
    }
    Transform3d transform = parent;
    if (auto transformNode = dynamic_cast<const TransformNode *>(node.get())) transform = transform * transformNode->matrix;
    if (auto colorNode = dynamic_cast<const ColorNode *>(node.get())) color = colorNode->color;
    for (const auto& child : node->children) roles(child, transform, color, output);
  }
  void attempt() {
    auto self = shared_from_this();
    if (cancelled || !window || !window->evaluationBinding->validate(stamp)) {
      done({Status::Cancelled, {}, "Inspection preparation cancelled"}); return;
    }
    if (GuiLocker::isLocked()) {
      QTimer::singleShot(10, delivery.get(), [self] { self->attempt(); }); return;
    }
    if (std::dynamic_pointer_cast<ThrownTogetherRenderer>(window->qglview->renderer)) {
      done({Status::Unsupported, {}, "Cross section clipping is unavailable in Thrown Together mode"}); return;
    }
    lock = std::make_unique<GuiLocker>();
    if (window->qglview->renderer == window->geomRenderer) finalGeometry = window->rootGeom;
    ++counts->jobs; if (finalGeometry) ++counts->reusedFinal; else ++counts->evaluations;
    elapsed.start();
    std::thread([self] { self->work(); }).detach();
  }
  void work() {
    ScopedProgressCallback progress([this] { if (cancelled) throw ProgressCancelException(); });
    InspectionPresentationController::Completion result;
    try {
      if (cancelled) throw ProgressCancelException();
      auto output = std::make_shared<InspectionMeshSet>();
      output->resultId = stamp.resultId.toStdString(); output->geometryGeneration = counts->jobs;
      if (scene && scene->root) {
        Tree tree(scene->root, scene->documentPath);
        GeometryEvaluator evaluator(tree);
        auto geometry = finalGeometry ? finalGeometry : evaluator.evaluateGeometry(*scene->root, true);
        add(geometry, *output, Transform3d::Identity(), Color4f(), InspectionPiece::Role::Solid);
        roles(scene->root, Transform3d::Identity(), Color4f(), *output);
      }
      if (cancelled) throw ProgressCancelException();
      result = {output->pieces.empty() ? Status::Empty : Status::Ready, output, {}};
    } catch (const ProgressCancelException&) { result = {Status::Cancelled, {}, "Inspection preparation cancelled"}; }
      catch (const std::domain_error& error) { result = {Status::Unsupported, {}, QString::fromUtf8(error.what())}; }
      catch (const std::exception& error) { result = {Status::Failed, {}, QString::fromUtf8(error.what())}; }
      catch (...) { result = {Status::Failed, {}, "Inspection preparation failed"}; }
    auto self = shared_from_this();
    QMetaObject::invokeMethod(delivery.get(), [self, result = std::move(result)]() mutable {
      self->lock.reset();
      self->counts->preparationMilliseconds = self->elapsed.nsecsElapsed()/1e6;
      self->done(std::move(result));
    }, Qt::QueuedConnection);
  }
};
}
ClipMeshJob::ClipMeshJob(MainWindow *window) : QObject(window), window(window), counts(std::make_shared<Metrics>()) {}
InspectionPresentationController::Cancel ClipMeshJob::start(std::shared_ptr<const Selection::EvaluatedScene> scene,
  Selection::Stamp stamp, InspectionPresentationController::Done done)
{
  if (cached && cachedResult == stamp.resultId) { done({cached->pieces.empty() ? Status::Empty : Status::Ready, cached, {}}); return {}; }
  auto work = std::make_shared<Work>(); work->window = window; work->scene = std::move(scene); work->stamp = stamp; work->counts = counts;
  QPointer<ClipMeshJob> owner(this);
  work->done = [owner, stamp, workDone = std::move(done)](InspectionPresentationController::Completion result) mutable {
    if (owner && result.meshes) { owner->cached = result.meshes; owner->cachedResult = stamp.resultId; }
    workDone(std::move(result));
  };
  QTimer::singleShot(0, work->delivery.get(), [work] { work->attempt(); });
  return [weak = std::weak_ptr<Work>(work)] { if (auto active = weak.lock()) active->cancelled = true; };
}
