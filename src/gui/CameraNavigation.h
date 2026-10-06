#pragma once

#include "glview/CameraPose.h"
#include "gui/NavigationSettings.h"
#include <QObject>
#include <QElapsedTimer>
#include <functional>

class QGLView;

// One operation boundary for viewport navigation. Paint observation never evaluates source.
class CameraNavigation : public QObject {
  Q_OBJECT
public:
  explicit CameraNavigation(QGLView *view);
  CameraPose current() const;
  void navigate(const CameraPose& pose);
  void orient(const Eigen::Vector3d& angles);
  void apply(const CameraPose& pose, bool finished = true);
  void observe();
  void cancel();
  bool permitted() const;
  std::function<bool()> allowNavigation;
  NavigationSettings settings;
  bool animating() const { return moving; }

signals:
  void displayChanged();
  void cancelled();

private:
  QGLView *view;
  CameraPose observed;
  CameraPose start, target;
  QElapsedTimer clock;
  quint64 generation = 0;
  bool moving = false;
  void tick(quint64 operation);
};
