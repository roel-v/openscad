#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

namespace SourceEditing {
enum class DiskReadStatus { Missing, Readable, Unreadable };
struct DiskSourceState {
  QString identity;
  DiskReadStatus status = DiskReadStatus::Missing;
  QByteArray sha256;
  QByteArray fileId;
  QString error;
  static DiskSourceState capture(const QString& path, QByteArray *bytes = nullptr);
  bool matches(const DiskSourceState& other) const;
};
struct SourceWriteResult {
  bool saved = false;
  bool conflict = false;
  DiskSourceState state;
  QString error;
};
// The optional callback enables deterministic I/O failure and competing-write checks.
SourceWriteResult writeSource(const QString& path, const QByteArray& bytes,
                              const DiskSourceState& expected,
                              const std::function<bool(QString&)>& beforeCommit = {});
}
