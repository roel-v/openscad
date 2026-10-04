#pragma once
#include "core/anchors/AnchorResolver.h"
#include "core/anchors/AnchorSourceEdits.h"
#include "gui/design/DesignDocument.h"
#include "gui/selection/InspectorDock.h"
#include "gui/selection/ViewportOverlays.h"
#include <QPointer>
class MainWindow;
namespace Anchors {
class Controller : public QObject {
  Q_OBJECT
public:
  explicit Controller(MainWindow *);
  ~Controller() override;
  ReferenceResolution resolve(const ReferenceKey&) const;
  bool selectReference(ReferenceKey);
  const std::optional<ReferenceKey>& selectedReference() const {return selected;}
  std::shared_ptr<References> references() const {return service;}
  std::optional<std::string> capture(const Selection::Hit&,std::optional<std::string> owner = {},std::string *error = nullptr);
  bool beginPick();
  Design::LifecycleResult saveCapture();
  void cancelCapture();
  std::optional<Design::Draft> captureDraft() const;
  bool allowSharedEdit(const ReferenceKey&);
  size_t declarationUses(const Design::Record&) const;
  Design::LifecycleResult editReference(const ReferenceKey&,const std::vector<LiteralChange>&);
  Design::PreparedEdit prepareEdit(const ReferenceKey&,const std::vector<LiteralChange>&,const Selection::Stamp&) const;
  void navigate(const ReferenceKey&);
  std::shared_ptr<const ReferenceScene> listedReferences();
private:
  QPointer<MainWindow> window;
  std::shared_ptr<References> service;
  std::optional<ReferenceKey> selected;
  std::set<ReferenceKey> sharedEdits;
  Selection::Stamp bound;
  Selection::InspectorDock::Token inspector;
  Selection::ViewportOverlays::Token overlay;
  struct Document {QPointer<Design::DesignDocument> owner;std::vector<Design::DesignDocument::Token> adapters;};
  std::map<QString,Document> documents;
  std::optional<std::string> captureToken;
  uint64_t tool=0;
  bool refreshing=false;
  void refresh();
  void refreshWidgets();
  Design::DesignDocument *document();
  std::shared_ptr<const Design::Record> record(const ReferenceKey&) const;
  Design::PreparedEdit insert(const Design::Draft&,const Selection::Stamp&) const;
  QWidget *buildInspector();
  void drawOverlay();
  void report(const std::string&);
};
std::string opaqueKey(const ReferenceKey&);
std::optional<ReferenceKey> referenceKey(const std::string&);
}
