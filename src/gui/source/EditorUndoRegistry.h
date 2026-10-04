#pragma once
#include <QObject>
#include <QPointer>
#include <functional>
#include <map>
#include <memory>

class EditorInterface;
namespace SourceEditing {
struct UndoParticipant {
  QPointer<QObject> lifetime;
  std::function<void()> undo, redo;
  bool valid() const { return lifetime && undo && redo; }
};
// Editor-local container tokens are shared by annotations and design lifecycle actions.
class EditorUndoRegistry : public QObject {
public:
  explicit EditorUndoRegistry(EditorInterface *editor);
  static EditorUndoRegistry *forEditor(EditorInterface&);
  int add(std::shared_ptr<const UndoParticipant>);
  void dispatch(int token, bool undo);
  void clear();
  size_t size() const { return participants.size(); }
private:
  struct Entry { std::shared_ptr<const UndoParticipant> participant; QMetaObject::Connection owner; };
  int nextToken = 1;
  std::map<int, Entry> participants;
};
}
