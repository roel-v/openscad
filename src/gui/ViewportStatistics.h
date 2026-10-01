#pragma once
#include <QLabel>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
class QGLView;

class ViewportStatistics : public QLabel {
  Q_OBJECT
public:
  explicit ViewportStatistics(QGLView *view);
  void frameCompleted();
  void refresh();
  void setStale(bool stale) { retained=stale; failed=false; refresh(); }
  void evaluationFailed() { failed=true; refresh(); }
  int sampleCount() const { return intervals.size(); }
private:
  QGLView *view;
  QElapsedTimer clock;
  qint64 previous=-1, lastText=-1;
  QVector<double> intervals;
  QTimer idle;
  bool retained=false;
  bool failed=false;
};
