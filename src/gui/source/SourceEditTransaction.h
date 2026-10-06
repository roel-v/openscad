#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <memory>
<<<<<<< HEAD
=======
#include <functional>
>>>>>>> feature/source-editing-foundation

class EditorInterface;

namespace SourceEditing {
struct UndoParticipant;
struct ByteEdit {
  qsizetype begin, end;
  QByteArray expected, replacement;
};
struct EditRequest {
  QString documentId;
  quint64 sourceRevision;
  QString sourceFingerprint;
  QVector<ByteEdit> edits;
  std::shared_ptr<const UndoParticipant> participant;
<<<<<<< HEAD
};
enum class EditError {
  None, WrongDocument, StaleSource, ReadOnly, InvalidRange, InvalidUtf8, Overlap, ExpectedTextMismatch, InvalidParticipant
=======
  // Rechecked after synchronous external-edit observers, before the undo group.
  std::function<bool()> contextCurrent;
};
enum class EditError {
  None, WrongDocument, StaleSource, ReadOnly, InvalidRange, InvalidUtf8, Overlap, ExpectedTextMismatch, InvalidParticipant, StaleContext
>>>>>>> feature/source-editing-foundation
};
struct EditResult {
  EditError error = EditError::None;
  bool changed = false;
  quint64 sourceRevision = 0;
  QString detail;
};
// Validates the entire original snapshot; output contains only changed edits, descending by offset.
EditResult validate(const QByteArray& source, const QString& documentId, quint64 revision,
                    const QString& fingerprint, bool readOnly, const EditRequest& request,
                    QVector<ByteEdit>& changes);
// GUI thread only. The caller retains a QPointer to its captured editor, never the active tab.
EditResult apply(EditorInterface& editor, const EditRequest& request);
qsizetype transformPosition(qsizetype position, const QVector<ByteEdit>& edits);
}
