#include "gui/source/EditorUndoRegistry.h"
#include "gui/Editor.h"
#include <limits>
#include <stdexcept>

namespace SourceEditing {
EditorUndoRegistry::EditorUndoRegistry(EditorInterface *editor) : QObject(editor) { setObjectName("editorUndoRegistry"); }
EditorUndoRegistry *EditorUndoRegistry::forEditor(EditorInterface& editor) {
  for (auto *child : editor.children()) if (auto *registry = dynamic_cast<EditorUndoRegistry *>(child)) return registry;
  return new EditorUndoRegistry(&editor);
}
int EditorUndoRegistry::add(std::shared_ptr<const UndoParticipant> participant) {
  if (!participant || !participant->valid()) throw std::invalid_argument("Undo participant is no longer live");
  if (nextToken == std::numeric_limits<int>::max()) throw std::overflow_error("Editor undo token space exhausted");
  const int token = nextToken++;
  auto connection = connect(participant->lifetime, &QObject::destroyed, this, [this, token] { participants.erase(token); });
  participants.emplace(token, Entry{std::move(participant), connection}); return token;
}
void EditorUndoRegistry::dispatch(int token, bool undo) {
  auto found = participants.find(token); if (found == participants.end()) return;
  auto participant = found->second.participant;
  if (!participant->valid()) { disconnect(found->second.owner); participants.erase(found); return; }
  if (undo) participant->undo(); else participant->redo();
}
void EditorUndoRegistry::clear() {
  for (auto& [token, entry] : participants) disconnect(entry.owner);
  participants.clear();
}
}
