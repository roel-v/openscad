#include "gui/NavigationCube.h"
#include "gui/CameraNavigation.h"
#include "gui/QGLView.h"
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QStylePainter>
#include <QStyleOptionToolButton>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {
class NavigationArrow : public QToolButton {
public:
  NavigationArrow(int direction,QWidget *parent) : QToolButton(parent),direction(direction) {}
protected:
  void paintEvent(QPaintEvent *) override {
    QStylePainter painter(this);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.text.clear();
    painter.drawComplexControl(QStyle::CC_ToolButton,option);
    painter.setRenderHint(QPainter::Antialiasing);
    const double side=std::min(width(),height());
    painter.translate((width()-side)/2,(height()-side)/2);
    painter.scale(side/20,side/20);
    const auto color=palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,QPalette::ButtonText);
    if (direction<2) {
      if (direction==0) {painter.translate(20,0);painter.scale(-1,1);}
      painter.setPen(QPen(color,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
      painter.setBrush(Qt::NoBrush);
      QPainterPath curve;
      curve.moveTo(14,16);
      curve.cubicTo(3,19,1,5,10,5);
      curve.cubicTo(13,5,15,6,16,9);
      painter.drawPath(curve);
      painter.drawPolyline(QPolygonF{QPointF(11,9),QPointF(16,9),QPointF(16,4)});
    } else {
      painter.translate(10,10);
      painter.rotate(direction==2 ? 0 : direction==3 ? 180 : direction==4 ? -90 : 90);
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawPolygon(QPolygonF{QPointF(0,-6),QPointF(6,4),QPointF(-6,4)});
    }
  }
private:
  int direction;
};
}

NavigationCube::NavigationCube(QGLView *view)
  : QWidget(view), view(view), navigation(view->navigation()), triangle(new QToolButton(view)),
    opacity(new QGraphicsOpacityEffect(this)), triangleOpacity(new QGraphicsOpacityEffect(triangle))
{
  setObjectName("navigationCube");
  setAccessibleName(tr("Navigation cube"));
  setToolTip(tr("Click a face, edge or corner; drag to orbit. Arrow keys rotate; Q/E roll. "
                "[ and ] select a target; Enter recalls it."));
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setGraphicsEffect(opacity);
  triangle->setGraphicsEffect(triangleOpacity);
  triangle->setText(QString::fromUtf8("\xe2\x96\xbe"));
  triangle->setAutoRaise(true);
  triangle->setStyleSheet("QToolButton { border: 0; padding: 0; background: transparent; }");
  triangle->setObjectName("viewportMenuTriangle");
  triangle->setAccessibleName(tr("Viewport settings"));
  triangle->setToolTip(tr("Viewport settings"));
  triangle->installEventFilter(this);
  connect(triangle, &QToolButton::clicked, this, &NavigationCube::menuRequested);
  const QStringList symbols{QString::fromUtf8("\xe2\x86\xb6"), QString::fromUtf8("\xe2\x86\xb7"),
                            QString::fromUtf8("\xe2\x96\xb2"), QString::fromUtf8("\xe2\x96\xbc"),
                            QString::fromUtf8("\xe2\x97\x80"), QString::fromUtf8("\xe2\x96\xb6")};
  const QStringList names{tr("Roll counterclockwise"), tr("Roll clockwise"), tr("Rotate up"),
                          tr("Rotate down"), tr("Rotate left"), tr("Rotate right")};
  for (int i=0; i<6; ++i) {
    auto button = new NavigationArrow(i,this);
    button->setText(symbols[i]);
    button->setToolTip(names[i]);
    button->setAccessibleName(names[i]);
    button->setAutoRaise(true);
    button->setStyleSheet("QToolButton { border: 0; padding: 0; background: transparent; }"
                         "QToolButton:hover { background: palette(midlight); }");
    button->setAutoRepeat(false);
    button->installEventFilter(this);
    connect(button, &QToolButton::clicked, this, [this,i] {
      const auto& s = navigation->settings;
      step(i<2 ? Eigen::Vector3d::UnitY() : i<4 ? Eigen::Vector3d::UnitX() : Eigen::Vector3d::UnitZ(),
           (i<2 ? (i%2 ? 1 : -1)*s.rollStep : (i%2 ? -1 : 1)*s.arrowStep));
    });
    arrows.push_back(button);
  }
  connect(navigation, &CameraNavigation::displayChanged, this, qOverload<>(&QWidget::update));
  connect(&navigation->settings, &NavigationSettings::changed, this, [this] { relayout(); activate(underMouse()); });
  connect(view, &QGLView::resized, this, &NavigationCube::relayout);
  view->installEventFilter(this);
  connect(&fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    opacity->setOpacity(value.toDouble());
    triangleOpacity->setOpacity(value.toDouble());
  });
  hoverDelay.setSingleShot(true);
  opacity->setOpacity(navigation->settings.idleOpacity);
  triangleOpacity->setOpacity(navigation->settings.idleOpacity);
  relayout();
}

void NavigationCube::relayout()
{
  cancelGesture();
  const auto& s = navigation->settings;
  const double scale = std::clamp(s.scale, 0.35, 3.0);
  resize(qRound(140*scale), qRound((184+std::max(0.0,s.arrowSize-20))*scale));
  move(std::clamp(qRound(view->width()-width()-s.cubeRight), 0, std::max(0,view->width()-width())),
       std::clamp(qRound(s.cubeTop), 0, std::max(0,view->height()-height())));
  const QPoint centers[] = {{42,10},{98,10},{70,30},{70,136},{10,82},{130,82}};
  const int arrowSide=std::max(1,qRound(s.arrowSize*scale));
  for (int i=0; i<6; ++i) {
    arrows[i]->setGeometry(std::clamp(qRound(centers[i].x()*scale-arrowSide/2.0),0,std::max(0,width()-arrowSide)),
                           std::clamp(qRound(centers[i].y()*scale-arrowSide/2.0),0,std::max(0,height()-arrowSide)),
                           arrowSide,arrowSide);
  }
  const int side = qRound(24*scale);
  QFont triangleFont=font();
  triangleFont.setPixelSize(std::max(1,qRound(14*scale)));
  triangle->setFont(triangleFont);
  triangle->setGeometry(std::clamp(qRound(view->width()-side-s.menuRight),0,std::max(0,view->width()-side)),
                        std::clamp(qRound(s.menuTop),0,std::max(0,view->height()-side)),side,side);
  setVisible(s.visible);
  triangle->setVisible(s.visible);
  raise(); triangle->raise();
  update();
}

void NavigationCube::updateContrast()
{
  if (!view->colorscheme) return;
  const auto background=ColorMap::getColor(*view->colorscheme,RenderColor::BACKGROUND_COLOR);
  const double brightness=0.2126*background.r()+0.7152*background.g()+0.0722*background.b();
  const QColor color=brightness<0.5 ? QColor(Qt::white) : QColor(Qt::black);
  auto setColor=[&](QToolButton *button) {
    auto colors=button->palette();
    if (colors.color(QPalette::ButtonText)==color) return;
    colors.setColor(QPalette::ButtonText,color);
    colors.setColor(QPalette::WindowText,color);
    button->setPalette(colors);
  };
  for (auto button:arrows) setColor(button);
  setColor(triangle);
}

std::vector<ProjectedNavigationFacet> NavigationCube::projected() const
{
  const auto& s = navigation->settings;
  const double scale = width()/140.0;
  return NavigationCubeGeometry::project(CameraPose::matrix(view->cam.object_rot),
                                         QPointF(70,80)*scale, s.cubeRadius*scale, s.chamfer);
}

void NavigationCube::paintEvent(QPaintEvent *)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const double scale = width()/140.0;
  QFont labelFont = font();
  labelFont.setPixelSize(std::max(7,qRound(navigation->settings.labelSize*scale)));
  labelFont.setBold(true);
  painter.setFont(labelFont);
  const bool dark = palette().color(QPalette::Window).lightness() < 128;
  const auto rotation = CameraPose::matrix(view->cam.object_rot);
  const auto facets = NavigationCubeGeometry::facets(navigation->settings.chamfer);
  for (const auto& face : projected()) {
    QColor fill = palette().color(QPalette::Button);
    fill = fill.darker(qRound(100 + 25*(1-face.facing)));
    if (face.id == hovered || (hasFocus() && face.id == keyboardTarget)) fill = palette().color(QPalette::Highlight);
    painter.setPen(QPen(dark ? QColor(175,183,195) : QColor(90,102,118), 0.8));
    painter.setBrush(fill);
    painter.drawPolygon(face.polygon);
    if (!face.label.isEmpty() && face.facing > 0.2) {
      painter.setPen(face.id == hovered ? palette().color(QPalette::HighlightedText) : palette().color(QPalette::ButtonText));
      const auto basis = CameraPose::matrix(CameraPose::upright(facets[face.id].normal));
      const Eigen::Vector3d right = rotation*basis.row(0).transpose();
      const Eigen::Vector3d up = rotation*basis.row(2).transpose();
      painter.save();
      painter.translate(face.center);
      painter.setWorldTransform(QTransform(right.x(),-right.z(),-up.x(),up.z(),0,0),true);
      const double extent=navigation->settings.cubeRadius*scale*(1-navigation->settings.chamfer);
      painter.drawText(QRectF(-extent,-8*scale,2*extent,16*scale),Qt::AlignCenter,face.label);
      painter.restore();
    }
  }
  const QPointF origin = QPointF(70,155+std::max(0.0,navigation->settings.arrowSize-20))*scale;
  const QColor colors[] = {QColor(210,55,55),QColor(35,155,65),QColor(40,110,235)};
  for (int i=0; i<3; ++i) {
    const Eigen::Vector3d p = rotation.col(i);
    const QPointF end = origin + QPointF(p.x(),-p.z())*14*scale;
    painter.setPen(QPen(colors[i],1.2));
    painter.drawLine(origin,end);
    const QPointF label = origin + QPointF(p.x(),-p.z())*21*scale;
    painter.drawText(QRectF(label-QPointF(6,6)*scale,QSizeF(12,12)*scale),Qt::AlignCenter,QString("XYZ").mid(i,1));
  }
}

void NavigationCube::snap(int id)
{
  const auto facets = NavigationCubeGeometry::facets(navigation->settings.chamfer);
  if (id>=0 && id<static_cast<int>(facets.size())) navigation->orient(CameraPose::upright(facets[id].normal));
}

void NavigationCube::step(const Eigen::Vector3d& axis, double degrees)
{
  navigation->orient(CameraPose::angles(Eigen::AngleAxisd(degrees*CameraPose::radians,axis).toRotationMatrix()*
                                      CameraPose::matrix(view->cam.object_rot)));
}

Eigen::Vector3d NavigationCube::trackball(const QPointF& point) const
{
  const double scale = width()/140.0;
  const double radius = navigation->settings.cubeRadius*scale*1.5;
  const double x = (point.x()-70*scale)/radius;
  const double z = (80*scale-point.y())/radius;
  const double r2 = x*x+z*z;
  return Eigen::Vector3d(x, -std::sqrt(std::max(0.0,1-r2)), z).normalized();
}

void NavigationCube::mousePressEvent(QMouseEvent *event)
{
  if (event->button()!=Qt::LeftButton || !navigation->permitted()) return;
  navigation->cancel();
  pressed = true; dragging = false; pressPoint = event->localPos();
  dragRotation = CameraPose::matrix(view->cam.object_rot);
  setFocus(Qt::MouseFocusReason);
  grabMouse();
  activate(true);
  event->accept();
}

void NavigationCube::mouseMoveEvent(QMouseEvent *event)
{
  const auto point = event->localPos();
  hovered = NavigationCubeGeometry::hit(projected(),point);
  if (pressed) {
    if ((point-pressPoint).manhattanLength() > navigation->settings.dragThreshold) dragging = true;
    if (dragging && navigation->settings.freeOrbit && navigation->permitted()) {
      Eigen::Quaterniond delta = Eigen::Quaterniond::FromTwoVectors(trackball(pressPoint),trackball(point));
      delta = Eigen::Quaterniond::Identity().slerp(navigation->settings.orbitSensitivity,delta);
      auto pose = navigation->current();
      pose.rotation = CameraPose::angles(delta.toRotationMatrix()*dragRotation);
      navigation->apply(pose,false);
    }
  }
  update();
  event->accept();
}

void NavigationCube::mouseReleaseEvent(QMouseEvent *event)
{
  if (!pressed || event->button()!=Qt::LeftButton) return;
  const bool didDrag = dragging;
  cancelGesture();
  if (!didDrag) snap(NavigationCubeGeometry::hit(projected(),event->localPos()));
  else if (navigation->settings.freeOrbit) navigation->apply(navigation->current());
  activate(underMouse());
  event->accept();
}

void NavigationCube::cancelGesture()
{
  pressed = dragging = false;
  if (mouseGrabber()==this) releaseMouse();
}

void NavigationCube::keyPressEvent(QKeyEvent *event)
{
  const auto& s = navigation->settings;
  switch (event->key()) {
  case Qt::Key_Left: step(Eigen::Vector3d::UnitZ(),s.arrowStep); break;
  case Qt::Key_Right: step(Eigen::Vector3d::UnitZ(),-s.arrowStep); break;
  case Qt::Key_Up: step(Eigen::Vector3d::UnitX(),s.arrowStep); break;
  case Qt::Key_Down: step(Eigen::Vector3d::UnitX(),-s.arrowStep); break;
  case Qt::Key_Q: step(Eigen::Vector3d::UnitY(),-s.rollStep); break;
  case Qt::Key_E: step(Eigen::Vector3d::UnitY(),s.rollStep); break;
  case Qt::Key_BracketLeft: keyboardTarget=(keyboardTarget+25)%26; update(); break;
  case Qt::Key_BracketRight: keyboardTarget=(keyboardTarget+1)%26; update(); break;
  case Qt::Key_Return: case Qt::Key_Enter: snap(keyboardTarget); break;
  case Qt::Key_Escape: cancelGesture(); navigation->cancel(); break;
  default: QWidget::keyPressEvent(event); return;
  }
  event->accept();
}

void NavigationCube::setPopupActive(bool active) { popupActive=active; activate(active || underMouse()); }

void NavigationCube::activate(bool active)
{
  if (!isVisible()) { fade.stop(); hoverDelay.stop(); return; }
  const auto& s = navigation->settings;
  active = active || popupActive || pressed || hasFocus() || triangle->hasFocus() ||
           (QApplication::focusWidget() && isAncestorOf(QApplication::focusWidget()));
  hoverDelay.stop();
  disconnect(&hoverDelay,nullptr,this,nullptr);
  connect(&hoverDelay,&QTimer::timeout,this,[this,active] {
    fade.stop();
    fade.setStartValue(opacity->opacity());
    fade.setEndValue(active ? navigation->settings.activeOpacity : navigation->settings.idleOpacity);
    fade.setDuration(active ? navigation->settings.fadeIn : navigation->settings.fadeOut);
    fade.start();
  });
  hoverDelay.start(active ? s.enterDelay : s.leaveDelay);
}

bool NavigationCube::event(QEvent *event)
{
  if (event->type()==QEvent::Enter || event->type()==QEvent::FocusIn) activate(true);
  if (event->type()==QEvent::Leave || event->type()==QEvent::FocusOut) {
    hovered=-1; activate(false); update();
  }
  if (event->type()==QEvent::FocusOut) cancelGesture();
  if (event->type()==QEvent::UngrabMouse) pressed=dragging=false;
  if (event->type()==QEvent::Hide || event->type()==QEvent::WindowDeactivate) {
    cancelGesture(); fade.stop(); hoverDelay.stop(); navigation->cancel();
  }
  return QWidget::event(event);
}

bool NavigationCube::eventFilter(QObject *object, QEvent *event)
{
  if (object==view && event->type()==QEvent::Resize) relayout();
  if (object!=view) {
    if (event->type()==QEvent::Enter || event->type()==QEvent::FocusIn) activate(true);
    if (event->type()==QEvent::Leave || event->type()==QEvent::FocusOut) activate(underMouse());
  }
  return QWidget::eventFilter(object,event);
}
