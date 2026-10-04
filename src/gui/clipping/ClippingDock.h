#pragma once
#include <QDockWidget>
class QTreeWidget;
class QCheckBox;
class QLineEdit;
class QDoubleSpinBox;
class QSlider;
class QLabel;
class QPushButton;
namespace Clipping {
class Controller;
class Dock : public QDockWidget {
  Q_OBJECT
public:
  explicit Dock(Controller *,QWidget *parent);
  void refresh();
private:
  Controller *controller;
  QTreeWidget *rows;
  QCheckBox *global,*helper;
  QLineEdit *name;
  QDoubleSpinBox *offset,*azimuth,*elevation;
  QDoubleSpinBox *origin[3];
  QSlider *slider;
  QLabel *status;
  QPushButton *promote,*remove,*pickPoint,*pickAlign,*flip;
  bool updating=false;
  void commitAngles();
};
}
