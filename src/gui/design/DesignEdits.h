#pragma once
#include "core/design/DesignObjects.h"
#include "gui/selection/SelectionTypes.h"
#include "gui/source/SourceEditTransaction.h"
#include <QPointer>
#include <Eigen/Geometry>

class EditorInterface;
class MainWindow;
namespace Design {
enum class ChangeKind { Label, Translation, Rotation, Offset, DeleteDeclaration, InsertRecord };
struct FieldChange {
  ChangeKind kind;
  std::string label;
  Vector3d translation = Vector3d::Zero();
  Eigen::Quaterniond rotation = Eigen::Quaterniond::Identity();
  double offset = 0;
};
struct PreparedEdit {
  QPointer<EditorInterface> editor;
  SourceEditing::EditRequest request;
  std::string error;
  explicit operator bool() const { return !editor.isNull() && error.empty(); }
};
class DesignEdits {
public:
  explicit DesignEdits(MainWindow *window);
  PreparedEdit prepare(const Record&, const std::vector<FieldChange>&, const Selection::Stamp&) const;
  PreparedEdit prepare(const std::string& recordId, const std::vector<FieldChange>&, const Selection::Stamp&) const;
  PreparedEdit insert(const std::vector<std::shared_ptr<const Record>>&, EditorInterface& destination,
                      const Selection::Stamp&) const;
private:
  QPointer<MainWindow> window;
};
}
