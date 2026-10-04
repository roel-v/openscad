#pragma once
#include "gui/selection/InspectorDock.h"
#include <QObject>
#include <QPointer>
#include <QSet>

class MainWindow;
namespace Design {
class SavedViewContribution : public QObject {
public:
  explicit SavedViewContribution(MainWindow *window);
  static SavedViewContribution *forWindow(MainWindow&);
private:
  QPointer<MainWindow> window;
  Selection::InspectorDock::Token contribution;
  QSet<QObject *> watched;
  QWidget *build(const Selection::Stamp&);
};
}
