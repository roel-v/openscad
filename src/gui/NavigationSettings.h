#pragma once
#include <QObject>
#include <QEasingCurve>

// Logical pixels and milliseconds. Phase 3 binds persistent baselines and session overrides here.
class NavigationSettings : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  double scale = 0.75;
  double cubeRight = 10, cubeTop = 26, menuRight = 8, menuTop = 5;
  double chamfer = 0.22, cubeRadius = 30, labelSize = 10, arrowSize = 20;
  double idleOpacity = 0.45, activeOpacity = 1;
  int enterDelay = 0, leaveDelay = 180, fadeIn = 120, fadeOut = 250;
  bool visible = true, animate = true, freeOrbit = true;
  int duration = 200;
  QEasingCurve::Type easing = QEasingCurve::InOutCubic;
  double rollStep = 90, arrowStep = 15, dragThreshold = 4, orbitSensitivity = 1;
signals:
  void changed();
};
