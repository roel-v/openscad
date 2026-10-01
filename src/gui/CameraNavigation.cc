#include "gui/CameraNavigation.h"
#include "gui/QGLView.h"

CameraNavigation::CameraNavigation(QGLView *view)
  : QObject(view), view(view), observed(CameraPose::from(view->cam))
{
}

CameraPose CameraNavigation::current() const { return CameraPose::from(view->cam); }

void CameraNavigation::navigate(const CameraPose& pose)
{
  cancel();
  apply(pose);
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

void CameraNavigation::cancel() { emit cancelled(); }
