#pragma once
#include "core/semantic/SemanticSourceEdits.h"
#include "gui/selection/SelectionTypes.h"
#include <QPointer>
#include <QWidget>
class MainWindow;
class QComboBox;
class QLabel;
class QScrollArea;
class EditorInterface;
namespace Selection { struct EvaluatedScene; }
namespace Semantic {
class Controller;
class ParameterPanel : public QWidget {
  Q_OBJECT
public:
  ParameterPanel(MainWindow *,Controller *,QWidget *parent);
  void refresh();
  void initializeStructureDialog();
  bool initializeCaptured(std::shared_ptr<const Selection::EvaluatedScene>,Selection::Stamp,
                          const QString& name,QString *detail=nullptr);
  // Same captured transaction used by the value-widget adapters and native tests.
  bool applyCaptured(std::shared_ptr<const Selection::EvaluatedScene>,Selection::Stamp,
                     TargetKey,const Parameter&,const Value&,bool reset=false,QString *detail=nullptr);
private:
  QPointer<MainWindow> window;
  QPointer<Controller> controller;
  QComboBox *mode;
  QLabel *status;
  QScrollArea *scroll;
  bool rebuilding=false;
  void addParameter(QWidget *,TargetKey,const Parameter&,bool inspectionOnly,
                    std::shared_ptr<const Selection::EvaluatedScene>,Selection::Stamp);
  EditorInterface *owner(const Scene&,const ParameterEdit&,QString&) const;
};
}
