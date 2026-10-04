#pragma once
#include "core/provenance/SourceProvenance.h"
#include "gui/selection/SelectionTypes.h"
#include "gui/selection/InspectorDock.h"
#include "gui/source/SourceEditTransaction.h"
#include <QObject>
#include <QPointer>

class MainWindow;
class EditorInterface;

namespace SourceProvenance {
struct Resolved {
  Selection::Hit hit;
  Mapping mapping;
  std::shared_ptr<const Result> result;
};
struct EditableTarget {
  QPointer<EditorInterface> editor;
  Selection::Stamp stamp;
  SourceEditing::EditRequest request;
};
enum class NavigationSite { Call, Definition };

// All entry points run on the GUI thread and validate the captured evaluation.
class ProvenanceService : public QObject {
public:
  explicit ProvenanceService(MainWindow *window);
  static ProvenanceService *forWindow(MainWindow *window);
  Resolved resolve(const Selection::Hit& hit) const;
  std::vector<uint64_t> navigationCandidates(const Resolved& resolved) const;
  std::optional<EditableTarget> editableSpan(const Resolved& resolved, uint64_t instance,
    const Span& span, const QByteArray& replacement, QString *detail = nullptr) const;
  bool validateTarget(const EditableTarget& target, QString *detail = nullptr) const;
  bool navigate(const Resolved& resolved, uint64_t instance, NavigationSite site,
    QString *detail = nullptr);
private:
  QPointer<MainWindow> window;
  Selection::InspectorDock::Token contribution;
  QWidget *build(const Selection::State& state, const Selection::Stamp& stamp);
};
}
