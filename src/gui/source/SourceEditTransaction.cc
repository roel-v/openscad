#include "gui/source/SourceEditTransaction.h"

#include <QThread>
#include <QPointer>
#include <algorithm>
#include <stdexcept>
#include "gui/Editor.h"

namespace SourceEditing {
EditResult validate(const QByteArray& source, const QString& documentId, quint64 revision,
                    const QString& fingerprint, bool readOnly, const EditRequest& request,
                    QVector<ByteEdit>& changes)
{
  changes.clear();
  auto failure = [revision](EditError error, const QString& detail) {
    return EditResult{error, false, revision, detail};
  };
  if (request.documentId != documentId) return failure(EditError::WrongDocument, "Document identity changed");
  if (request.sourceRevision != revision || request.sourceFingerprint != fingerprint)
    return failure(EditError::StaleSource, "Source revision or fingerprint changed");
  if (readOnly) return failure(EditError::ReadOnly, "Document is read-only");
  auto ordered = request.edits;
  std::sort(ordered.begin(), ordered.end(), [](const ByteEdit& a, const ByteEdit& b) { return a.begin < b.begin; });
  const auto boundary = [&source](qsizetype position) {
    return position == source.size() || (static_cast<unsigned char>(source.at(position)) & 0xc0) != 0x80;
  };
  qsizetype previousStart = -1, previousEnd = -1;
  for (const auto& edit : ordered) {
    if (edit.begin < 0 || edit.end < edit.begin || edit.end > source.size())
      return failure(EditError::InvalidRange, "Range lies outside the original source");
    if (!boundary(edit.begin) || !boundary(edit.end) ||
        QString::fromUtf8(edit.replacement).toUtf8() != edit.replacement)
      return failure(EditError::InvalidUtf8, "Range boundary or replacement is not valid UTF-8");
    if (edit.begin == previousStart || edit.begin < previousEnd)
      return failure(EditError::Overlap, "Ranges overlap or have duplicate starts");
    if (source.mid(edit.begin, edit.end - edit.begin) != edit.expected)
      return failure(EditError::ExpectedTextMismatch, "Original range does not match expected bytes");
    previousStart = edit.begin;
    previousEnd = edit.end;
  }
  for (auto it = ordered.crbegin(); it != ordered.crend(); ++it)
    if (it->expected != it->replacement) changes.append(*it);
  return {EditError::None, !changes.isEmpty(), revision, {}};
}

EditResult apply(EditorInterface& editor, const EditRequest& request)
{
  if (QThread::currentThread() != editor.thread())
    throw std::logic_error("Source transactions must run on the editor GUI thread");
  QVector<ByteEdit> changes;
  auto result = validate(editor.toPlainText().toUtf8(), editor.documentId(), editor.sourceRevision,
                         editor.sourceFingerprint, editor.isSourceReadOnly(), request, changes);
  if (result.error == EditError::None && result.changed) {
    QPointer<EditorInterface> guard(&editor);
    editor.prepareExternalEdit();
    if (!guard)
      return {EditError::WrongDocument, false, result.sourceRevision, "Document closed before editing"};
    // Synchronous observers may close an insertion group, but can also change or close the document.
    // Revalidate the complete original request before using its byte offsets.
    result = validate(editor.toPlainText().toUtf8(), editor.documentId(), editor.sourceRevision,
                      editor.sourceFingerprint, editor.isSourceReadOnly(), request, changes);
    if (result.error == EditError::None && result.changed) editor.applyValidatedByteEdits(changes);
  }
  result.sourceRevision = editor.sourceRevision;
  return result;
}

qsizetype transformPosition(qsizetype position, const QVector<ByteEdit>& edits)
{
  qsizetype shift = 0;
  // Input is descending, so traverse in original source order.
  for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
    if (position < it->begin) break;
    if (position < it->end || (it->begin == it->end && position == it->begin))
      return it->begin + shift + it->replacement.size();
    shift += it->replacement.size() - (it->end - it->begin);
  }
  return position + shift;
}
}
