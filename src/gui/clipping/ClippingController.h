#pragma once
#include "gui/clipping/ClipMeshJob.h"
#include "gui/design/DesignDocument.h"
#include "gui/selection/ViewportOverlays.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignReferences.h"
#include "glview/ClippingState.h"
#include <QPointer>
#include <set>
class MainWindow;
class EditorInterface;
namespace Clipping {
class Dock;
class Controller : public QObject {
  Q_OBJECT
public:
  explicit Controller(MainWindow *window);
  ~Controller() override;
  static Controller *forWindow(MainWindow&);
  Design::Records records() const;
  Design::Records displayRecords() const;
  bool sourceStateCurrent() const;
  std::optional<Transform3d> frame(const std::string& id, QString *error=nullptr) const;
  BoundingBox bounds() const;
  bool evaluationCurrent() const;
  std::optional<std::string> createSection();
  bool enable(const std::string& id,bool enabled);
  bool enabled(const std::string& id) const { return enabledIds.count(id); }
  const std::set<std::string>& enabledSections() const { return enabledIds; }
  bool globalEnabled() const { return global; }
  void setGlobalEnabled(bool);
  bool helperEnabled(const std::string& id) const { return helperIds.count(id); }
  void setHelperEnabled(const std::string& id,bool);
  void select(const std::string& id);
  const std::string& selectedId() const { return selected; }
  QString documentId() const { return ownerDocumentId; }
  bool editable(const std::string& id,QString *reason=nullptr) const;
  bool beginGesture(const std::string& id);
  bool preview(const std::vector<Design::FieldChange>&);
  bool finishGesture();
  void cancelGesture();
  bool edit(const std::string&,const std::vector<Design::FieldChange>&);
  bool flip(const std::string&);
  bool setCutAwayDirection(const std::string&,const Vector3d&);
  bool setOrigin(const std::string&,const Vector3d&);
  bool remove(std::string);
  bool promote(const std::string&);
  void pick(const std::string&,bool align);
  bool applyHit(const std::string&,const Selection::Hit&,bool align);
  QString feedback() const { return message; }
  ClipCapabilities capabilities() const;
  const ClipMeshJob::Metrics& metrics() const { return jobs->metrics(); }
  std::shared_ptr<const ClipState> clippingState() const { return state; }
  Dock *dock=nullptr;
signals:
  void changed();
private:
  QPointer<MainWindow> window;
  QPointer<EditorInterface> owner;
  QString ownerDocumentId;
  QPointer<Design::DesignDocument> document;
  QMetaObject::Connection documentConnection;
  ClipMeshJob *jobs;
  std::shared_ptr<ClipState> state=std::make_shared<ClipState>();
  InspectionPresentationController::Token provider,effect,request;
  Selection::ViewportOverlays::Token overlay;
  std::set<std::string> enabledIds,helperIds,sessionIds;
  std::string selected,draft;
  std::optional<ClipState> priorGesture;
  Selection::Stamp gestureStamp;
  uint64_t tool=0;
  bool global=true,refreshing=false,closing=false;
  QString message,settingsKey;
  void bind();
  void refresh();
  void savePersonal() const;
  void fail(const QString&);
  Design::ResolveContext context(const Design::Records&) const;
  std::vector<Design::FieldChange> poseChanges(const std::string&,const Vector3d& point,
                                             const Vector3d& normal,const Vector3d& up) const;
  void drawHelper(const Selection::OverlayContext&);
  void prepareHelper();
  std::optional<Transform3d> helperPose;
  double planeSize=1;
};
}
