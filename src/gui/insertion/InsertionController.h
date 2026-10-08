// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/InsertionPlanner.h"
#include "core/insertion/PlacementDraft.h"
#include "core/providers/ProviderCommands.h"
#include "gui/insertion/InsertionPreviewProcess.h"
#include "gui/InspectionPresentationController.h"
#include "gui/selection/ViewportOverlays.h"
#include <QPointer>
#include <QTimer>
class MainWindow;
class EditorInterface;
class QDockWidget;
class QListWidget;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QFormLayout;
class QDoubleSpinBox;
namespace Selection {struct EvaluatedScene;}
namespace Insertion {
class Controller:public QObject {
  Q_OBJECT
public:
  enum class State {Idle,Preparing,Picking,Choosing,Configuring,Pending,Ready,Committing};
  explicit Controller(MainWindow *);
  ~Controller() override;
  bool add();
  bool begin(const Selection::Hit&);
  bool choose(const std::string&);
  bool selectTarget(size_t);
  bool openSource(size_t);
  bool setValues(const Providers::ParameterValues&);
  bool setPlacement(const Vector3d&,double,bool);
  bool setNamedAnchor(std::optional<Anchors::ReferenceKey>);
  void approveShared(bool);
  void cancel();
  bool commit();
  void reloadProviders();
  State state()const{return currentState;}
  QString message()const{return statusText;}
  const std::vector<TargetOption>& targets()const{return targetOptions;}
  const std::optional<EditPlan>& editPlan()const{return plan;}
  const std::optional<PreviewResponse>& proof()const{return result;}
  const std::optional<PlacementDraft>& draftPlacement()const{return placement;}
  std::shared_ptr<const Providers::RegistrySnapshot> catalogs()const{return registry;}
signals:
  void changed();
private:
  QPointer<MainWindow> window;
  QPointer<EditorInterface> rootEditor,targetEditor;
  Selection::Stamp stamp;
  std::shared_ptr<const Selection::EvaluatedScene> scene;
  std::optional<SourceInventory> inventory;
  std::shared_ptr<EvaluationInputs> capturedInputs;
  NativeConfiguration configuration;
  Providers::DiscoveredProviders discovered;
  std::shared_ptr<const Providers::RegistrySnapshot> registry;
  Providers::LibraryResolutionContext bindingContext;
  std::optional<Providers::ProviderBinding> binding;
  std::optional<Providers::GeneratedFragment> fragment;
  std::vector<TargetOption> targetOptions;
  std::vector<Anchors::ReferenceKey> anchorKeys;
  std::optional<SourceTarget> target;
  std::optional<Selection::Hit> hit;
  Vector3d cameraUp;
  std::optional<PlacementDraft> placement;
  std::optional<EditPlan> plan;
  std::optional<PreviewResponse> result;
  std::shared_ptr<const InspectionMeshSet> ghost;
  QPointer<PreviewProcess> process;
  InspectionPresentationController::DraftToken display;
  Selection::ViewportOverlays::Token overlay;
  uint64_t tool=0,serial=0;
  uint64_t preparingRevision=0;
  State currentState=State::Idle;
  enum class Drag {None,Move,Spin} drag=Drag::None;
  QString statusText;
  QTimer debounce,watch,elapsed;
  QDockWidget *dock;
  QListWidget *palette;
  QLineEdit *search;
  QComboBox *destinations,*anchors;
  QCheckBox *shared;
  QWidget *parameters;
  QFormLayout *fields;
  std::map<std::string,QWidget *> controls;
  QDoubleSpinBox *offsets[3],*spin;
  QCheckBox *flip;
  QLabel *scope,*status;
  QPushButton *commitButton;
  bool refreshing=false,closing=false;
  void setState(State,QString);
  void refreshPalette();
  void buildParameters();
  void refreshScope();
  void refreshControls();
  std::map<std::string,EditableDocument> documents()const;
  Providers::LibraryResolutionContext context(const std::string&,std::shared_ptr<EvaluationInputs>)const;
  bool current(bool provider=false)const;
  bool catalogsCurrent()const;
  void parametersChanged();
  void schedule(bool base=false);
  void launch();
  void accept(uint64_t,PreviewResponse);
  void stopProcess();
  void armDrag(Drag);
  bool startDrag(QPointF);
  Result<Vector2d> planePoint(QPointF)const;
  void drawGhost();
  bool eventFilter(QObject *,QEvent *) override;
};
}
