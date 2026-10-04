#pragma once
#include "gui/semantic/ModelTreeModel.h"
#include "gui/semantic/SemanticTargetService.h"
#include "gui/InspectionPresentationController.h"
#include "gui/selection/InspectorDock.h"
#include "gui/selection/ViewportOverlays.h"
#include <QPointer>
class MainWindow;
class QDockWidget;
class QTreeView;
class QLabel;
class QTabWidget;
class EditorInterface;
namespace Selection { struct EvaluatedScene; }
namespace Semantic {
class ParameterPanel;
class Controller : public QObject {
  Q_OBJECT
public:
  explicit Controller(MainWindow *);
  ~Controller() override;
  TargetService *targetService() const { return service; }
  ModelTreeModel *model() const { return treeModel; }
  QTreeView *treeView() const { return tree; }
  std::optional<TargetKey> selectedTarget() const;
  bool select(TargetKey);
  bool setHidden(const std::string&,bool);
  bool setSolo(std::optional<std::string>);
  Visibility visibility() const;
  uint64_t meshJobs() const { return preparedJobs; }
  std::optional<std::string> pathForDraw(uint64_t) const;
  QWidget *customizerWidget(EditorInterface *);
  ParameterPanel *parameterPanel() const;
signals:
  void targetChanged();
private:
  struct DocumentState { Visibility visibility; std::optional<TargetKey> selected; bool shown=false; };
  QPointer<MainWindow> window;
  QDockWidget *dock;
  QTreeView *tree;
  ModelTreeModel *treeModel;
  QLabel *status;
  TargetService *service;
  std::shared_ptr<const Selection::EvaluatedScene> scene;
  Selection::Stamp sourceStamp;
  std::map<QString,DocumentState> documents;
  struct CustomizerHost { QPointer<EditorInterface> editor; QPointer<QTabWidget> tabs; QPointer<ParameterPanel> panel; };
  std::map<QString,CustomizerHost> customizers;
  std::map<uint64_t,std::string> drawPaths;
  InspectionPresentationController::Token provider,request;
  Selection::InspectorDock::Token inspector;
  Selection::ViewportOverlays::Token overlay;
  std::shared_ptr<QObject> dispatcher;
  uint64_t preparedJobs=0,maskSerial=0,lastMask=0,lastGeneration=0;
  bool refreshing=false,applying=false,rebinding=false;
  bool current() const;
  bool prepared() const;
  void refresh();
  void applyMask();
  void rebind();
  void picked(const Selection::State&);
  void navigate(bool definition);
  void fit();
  void createSection();
  void exportSelected();
  QWidget *buildInspector();
  void drawHighlight();
  void refreshParameters();
  void releaseCustomizer(const QString&);
};
}
