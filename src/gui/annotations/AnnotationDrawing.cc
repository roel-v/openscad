#include "AnnotationDrawing.h"
#include <QJsonObject>
#include <QLineF>
#include <QTransform>
#include <cmath>

namespace AgentAnnotations {
void Drawing::begin(QPointF point, QSize size, QColor color)
{
  if (size.isEmpty() || strokes.size() >= 1024) return;
  pressed = true;
  active = {color, {}, {}};
  move(point, size);
}
void Drawing::move(QPointF point, QSize size)
{
  if (!pressed) return;
  if (!QRectF(QPointF(), size).contains(point)) {
    pressed = false;
    active = {};
    return;
  }
  QPointF normalized(point.x() / size.width(), point.y() / size.height());
  if (active.points.size() >= 5000) return;
  if (!active.points.isEmpty()) {
    auto last = active.points.last();
    if (QLineF(QPointF(last.x() * size.width(), last.y() * size.height()), point).length() < 1) return;
    active.path.lineTo(normalized);
  } else active.path.moveTo(normalized);
  active.points.append(normalized);
}
void Drawing::end(QSize size)
{
  if (!pressed) return;
  pressed = false;
  double length = 0;
  for (int i = 1; i < active.points.size(); ++i) {
    auto delta = active.points[i] - active.points[i - 1];
    length += std::hypot(delta.x() * size.width(), delta.y() * size.height());
  }
  int count = active.points.size();
  for (const auto& stroke : strokes) count += stroke.points.size();
  if (length >= 3 && count <= 50000) {
    strokes.append(active);
    redoStrokes.clear();
  }
  active = {};
}
void Drawing::undo()
{
  pressed = false;
  active = {};
  if (!strokes.isEmpty()) redoStrokes.append(strokes.takeLast());
}
void Drawing::redo()
{
  if (!pressed && !redoStrokes.isEmpty()) strokes.append(redoStrokes.takeLast());
}
void Drawing::paint(QPainter& painter, QSize size, qreal scale) const
{
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  QTransform transform;
  transform.scale(size.width(), size.height());
  auto paintStroke = [&](const Stroke& stroke) {
    auto path = transform.map(stroke.path);
    painter.setPen(QPen(QColor(20, 20, 20, 230), 5 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.setPen(QPen(stroke.color, 3 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
  };
  for (const auto& stroke : strokes) paintStroke(stroke);
  if (pressed) paintStroke(active);
  painter.restore();
}
QJsonArray Drawing::json() const
{
  QJsonArray array;
  for (const auto& stroke : strokes) {
    QJsonArray points;
    for (const auto& point : stroke.points) points.append(QJsonArray{point.x(), point.y()});
    array.append(QJsonObject{{"color", stroke.color.name()},
                             {"width", 3},
                             {"width_units", "logical_pixels"},
                             {"points", points}});
  }
  return array;
}
}  // namespace AgentAnnotations
