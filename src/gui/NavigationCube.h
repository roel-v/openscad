#pragma once
#include "gui/NavigationCubeGeometry.h"
#include <QWidget>
#include <QVariantAnimation>
#include <QTimer>
#include <QToolButton>
#include <vector>

class QGLView;
class CameraNavigation;
class QGraphicsOpacityEffect;

class NavigationCube : public QWidget {
  Q_OBJECT
public:
  explicit NavigationCube(QGLView *view);
  void relayout();
  void snap(int id);
  void step(const Eigen::Vector3d& axis, double degrees);
  QToolButton *menuTriangle() const { return triangle; }
  void setPopupActive(bool active);
  std::vector<ProjectedNavigationFacet> projected() const;

signals:
  void menuRequested();

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  bool event(QEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;

private:
  QGLView *view;
  CameraNavigation *navigation;
  QToolButton *triangle;
  std::vector<QToolButton *> arrows;
  QGraphicsOpacityEffect *opacity;
  QGraphicsOpacityEffect *triangleOpacity;
  QVariantAnimation fade;
  QTimer hoverDelay;
  bool pressed = false, dragging = false, popupActive = false;
  QPointF pressPoint;
  Eigen::Matrix3d dragRotation;
  int hovered = -1, keyboardTarget = 0;
  void activate(bool active);
  void cancelGesture();
  Eigen::Vector3d trackball(const QPointF& point) const;
};
