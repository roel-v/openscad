#pragma once
#include <QColor>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QVector>

namespace AgentAnnotations {
struct Stroke {
  QColor color;
  QVector<QPointF> points;
  QPainterPath path;
};
class Drawing
{
public:
  QVector<Stroke> strokes, redoStrokes;
  Stroke active;
  bool pressed = false;
  bool marked() const { return pressed || !strokes.isEmpty(); }
  void begin(QPointF point, QSize size, QColor color);
  void move(QPointF point, QSize size);
  void end(QSize size);
  void undo();
  void redo();
  void navigation()
  {
    if (!marked()) redoStrokes.clear();
  }
  void paint(QPainter& painter, QSize size, qreal scale = 1) const;
  QJsonArray json() const;
};
}  // namespace AgentAnnotations
