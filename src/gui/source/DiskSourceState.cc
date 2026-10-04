#include "gui/source/DiskSourceState.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace SourceEditing {
DiskSourceState DiskSourceState::capture(const QString& path, QByteArray *bytes)
{
  if (bytes) bytes->clear();
  QFileInfo info(path);
  auto canonical = info.canonicalFilePath();
  DiskSourceState state;
  state.identity = canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
  if (!info.exists() && !info.isSymLink()) return state;
  QFile file(path);
  state.status = DiskReadStatus::Unreadable;
  if (!file.open(QIODevice::ReadOnly)) { state.error = file.errorString(); return state; }
#ifdef Q_OS_WIN
  BY_HANDLE_FILE_INFORMATION infoByHandle{};
  const auto nativeHandle = _get_osfhandle(static_cast<int>(file.handle()));
  if (nativeHandle == -1 || !GetFileInformationByHandle(reinterpret_cast<HANDLE>(nativeHandle), &infoByHandle)) {
    state.error = "Unable to identify the source file";
    return state;
  }
  state.fileId = QByteArray::number(static_cast<qulonglong>(infoByHandle.dwVolumeSerialNumber)) + ':' +
    QByteArray::number(static_cast<qulonglong>(infoByHandle.nFileIndexHigh)) + ':' +
    QByteArray::number(static_cast<qulonglong>(infoByHandle.nFileIndexLow));
#else
  struct stat fileStat{};
  if (fstat(static_cast<int>(file.handle()), &fileStat) != 0) {
    state.error = "Unable to identify the source file";
    return state;
  }
  state.fileId = QByteArray::number(static_cast<qulonglong>(fileStat.st_dev)) + ':' +
    QByteArray::number(static_cast<qulonglong>(fileStat.st_ino));
#endif
  auto content = file.readAll();
  if (file.error() != QFile::NoError) { state.error = file.errorString(); return state; }
  state.status = DiskReadStatus::Readable;
  state.sha256 = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
  if (bytes) *bytes = content;
  return state;
}

bool DiskSourceState::matches(const DiskSourceState& other) const
{
  return status != DiskReadStatus::Unreadable && other.status != DiskReadStatus::Unreadable &&
    identity == other.identity && status == other.status && sha256 == other.sha256 && fileId == other.fileId;
}

SourceWriteResult writeSource(const QString& path, const QByteArray& bytes,
                              const DiskSourceState& expected,
                              const std::function<bool(QString&)>& beforeCommit)
{
  SourceWriteResult result;
  const auto unchanged = [&] {
    result.state = DiskSourceState::capture(path);
    if (expected.matches(result.state)) return true;
    result.conflict = true;
    result.error = "The source destination changed after approval. No source was saved.";
    return false;
  };
  if (!unchanged()) return result;
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush()) {
    result.error = file.errorString();
    file.cancelWriting();
    return result;
  }
  if ((beforeCommit && !beforeCommit(result.error)) || !unchanged()) {
    file.cancelWriting();
    return result;
  }
  if (!file.commit()) { result.error = file.errorString(); return result; }
  result.saved = true;
  result.state = DiskSourceState::capture(path);
  // The baseline belongs to the bytes we committed, even if another writer wins immediately afterward.
  result.state.status = DiskReadStatus::Readable;
  result.state.sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  return result;
}
}
