#pragma once
#include "gui/selection/SelectionTypes.h"
#include <QObject>
#include <QPointer>
#include <QPointF>
class MainWindow;

namespace Selection {
class SelectionService : public QObject {
  Q_OBJECT
public:
  explicit SelectionService(MainWindow *window);
  PickResult pick(QPointF logicalPixel, const Stamp& expected);
  bool selectObject(const ObjectKey& object, const Stamp& expected);
  void clear();
  const State& state() const { return selected; }
signals:
  void selectionChanged(const Selection::State& state);
private:
  QPointer<MainWindow> window;
  quint64 cameraSerial = 0, presentationSerial = 0;
  State selected;
};
}
