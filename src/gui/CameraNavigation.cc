#include "gui/CameraNavigation.h"
#include "gui/QGLView.h"
#include <QTimer>
#include <algorithm>

CameraNavigation::CameraNavigation(QGLView *view)
  : QObject(view), view(view), observed(CameraPose::from(view->cam))
{
}

CameraPose CameraNavigation::current() const { return CameraPose::from(view->cam); }

void CameraNavigation::navigate(const CameraPose& pose)
{
  cancel();
  if (!permitted() || !pose.valid()) return;
  if (!settings.animate || settings.duration <= 0 || !view->isVisible()) {
    apply(pose);
    return;
  }
  start = current();
  if (!start.valid()) { apply(pose); return; }
  target = pose;
  start.projection = pose.projection;
  apply(start, false);
  moving = true;
  clock.start();
  tick(generation);
}

void CameraNavigation::tick(quint64 operation)
{
  if (operation != generation || !moving) return;
  if (!permitted() || !view->isVisible() || !(current() == observed)) {
    observe();
    cancel();
    return;
  }
  const double progress = std::min(1.0, double(clock.elapsed()) / std::max(1, settings.duration));
  if (progress >= 1) {
    moving = false;
    apply(target);
    return;
  }
  const double t = QEasingCurve(settings.easing).valueForProgress(progress);
  auto pose = start;
  const Eigen::Quaterniond first(CameraPose::matrix(start.rotation));
  const Eigen::Quaterniond last(CameraPose::matrix(target.rotation));
  pose.rotation = CameraPose::angles(first.slerp(t, last).normalized().toRotationMatrix());
  pose.translation = (1-t)*start.translation + t*target.translation;
  pose.distance = std::exp((1-t)*std::log(start.distance) + t*std::log(target.distance));
  pose.fov = (1-t)*start.fov + t*target.fov;
  apply(pose, false);
  QTimer::singleShot(16, this, [this, operation] { tick(operation); });
}

void CameraNavigation::orient(const Eigen::Vector3d& angles)
{
  auto pose = current();
  pose.rotation = angles;
  navigate(pose);
}

bool CameraNavigation::permitted() const
{
  return !view->cam.locked && (!allowNavigation || allowNavigation());
}

void CameraNavigation::apply(const CameraPose& pose, bool finished)
{
  if (!permitted() || !pose.valid()) return;
  pose.apply(view->cam);
  observed = current();
  emit displayChanged();
  view->update();
  if (finished) emit view->cameraChanged();
}

void CameraNavigation::observe()
{
  if (!(current() == observed)) {
    cancel();
    observed = current();
    emit displayChanged();
  }
}

void CameraNavigation::cancel()
{
  ++generation;
  moving = false;
  emit cancelled();
}
