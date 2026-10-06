#pragma once
#include <QDockWidget>
#include "geometry/linalg.h"
#include <string>
class QTreeWidget;
class QCheckBox;
class QLineEdit;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QComboBox;
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
  QComboBox *directionPreset;
  QWidget *directionFields;
  QLineEdit *direction[3];
  std::string customDirectionId;
  QString directionDocumentId;
  std::string directionSelectionId;
  Vector3d displayedCutAway=Vector3d::Zero();
  QDoubleSpinBox *origin[3];
  QLabel *originLabel[3];
  QLabel *status;
  QPushButton *promote,*remove,*pickPoint,*pickAlign,*flip;
  bool updating=false;
  bool committingDirection=false;
  void commitDirection();
  void updateCoordinateVisibility();
};
}
