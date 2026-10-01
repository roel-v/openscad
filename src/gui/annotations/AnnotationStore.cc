#include "AnnotationStore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <cmath>
#include <zip.h>

namespace AgentAnnotations {
namespace {
bool fail(QString *error, const QString& message)
{
  if (error) *error = message;
  return false;
}
QByteArray fileHash(const QString& path)
{
  QFile file(path);
  if (file.size() > Store::MaxArchive) return {};
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return hash.result().toHex();
}
bool readEntry(zip_t *archive, zip_uint64_t index, qint64 limit, QByteArray& data)
{
  zip_stat_t stat;
  zip_stat_init(&stat);
  if (zip_stat_index(archive, index, 0, &stat) || stat.size > static_cast<zip_uint64_t>(limit) ||
      stat.encryption_method != ZIP_EM_NONE)
    return false;
  zip_file_t *file = zip_fopen_index(archive, index, 0);
  if (!file) return false;
  data.resize(static_cast<int>(stat.size));
  qint64 offset = 0;
  while (offset < data.size()) {
    auto count = zip_fread(file, data.data() + offset, data.size() - offset);
    if (count <= 0) {
      zip_fclose(file);
      return false;
    }
    offset += count;
  }
  char extra;
  bool ok = zip_fread(file, &extra, 1) == 0;
  return zip_fclose(file) == 0 && ok;
}
}  // namespace

Store::Store(QObject *parent)
  : QObject(parent), sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
  currentState = savedState = signature();
}
QByteArray Store::hash(const QByteArray& bytes)
{
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
void Store::renewSession()
{
  sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  clearHistory();
  emit sessionChanged();
}
QByteArray Store::signature() const
{
  QJsonArray array;
  for (const auto& record : items) array.append(record.metadata);
  return hash(QJsonDocument(array).toJson(QJsonDocument::Compact));
}
bool Store::dirty() const
{
  return savedState != currentState;
}
const Record *Store::find(const QString& id) const
{
  for (const auto& record : items)
    if (record.id() == id) return &record;
  return nullptr;
}

bool Store::validate(const Record& record, QString *error)
{
  const auto& m = record.metadata;
  if (record.id().size() != 36 || QUuid(record.id()).isNull() ||
      QUuid(record.id()).toString(QUuid::WithoutBraces) != record.id().toLower() ||
      !m.value("comment").isString() || m.value("comment").toString().size() > 65536 ||
      !QDateTime::fromString(m.value("created_utc").toString(), Qt::ISODateWithMs).isValid())
    return fail(error, "Invalid annotation identity, timestamp or comment.");
  if (QJsonDocument(m).toJson(QJsonDocument::Compact).size() > 1024 * 1024)
    return fail(error, "Annotation metadata exceeds 1 MiB.");
  if (record.png.isEmpty() || record.png.size() > MaxPng ||
      m.value("png_sha256").toString().toLatin1() != hash(record.png))
    return fail(error, "Invalid PNG size or checksum.");
  QBuffer buffer;
  buffer.setData(record.png);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, "PNG");
  const QSize size = reader.size();
  if (size.width() <= 0 || size.height() <= 0 || qint64(size.width()) * size.height() > 64000000 ||
      m.value("width").toInt() != size.width() || m.value("height").toInt() != size.height() ||
      reader.read().isNull())
    return fail(error, "Invalid PNG dimensions or data.");
  const auto camera = m.value("camera").toObject();
  for (const auto& key : {"translation", "rotation"}) {
    const auto array = camera.value(key).toArray();
    if (array.size() != 3) return fail(error, "Invalid camera vector.");
    for (const auto& value : array)
      if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return fail(error, "Invalid camera value.");
  }
  double distance = camera.value("distance").toDouble(-1), fov = camera.value("fov").toDouble(-1);
  if (!std::isfinite(distance) || distance <= 0 || !std::isfinite(fov) || fov <= 0 || fov >= 180 ||
      (camera.value("projection") != "perspective" && camera.value("projection") != "orthographic"))
    return fail(error, "Invalid camera projection.");
  if (!m.value("strokes").isArray() || !m.value("result").isObject())
    return fail(error, "Missing capture metadata.");
  int points = 0;
  for (const auto& value : m.value("strokes").toArray()) {
    const auto stroke = value.toObject();
    const QString color = stroke.value("color").toString();
    if (color.size() != 7 || color[0] != '#' || !QColor(color).isValid())
      return fail(error, "Invalid stroke color.");
    const double width = stroke.value("width").toDouble();
    if (!std::isfinite(width) || width <= 0 || width > 100) return fail(error, "Invalid stroke width.");
    auto vertices = stroke.value("points").toArray();
    if (vertices.size() < 2 || (points += vertices.size()) > 50000)
      return fail(error, "Invalid stroke count.");
    for (const auto& vertex : vertices) {
      auto xy = vertex.toArray();
      if (xy.size() != 2) return fail(error, "Invalid stroke coordinate.");
      for (const auto& coordinate : xy) {
        double number = coordinate.toDouble(-1);
        if (!coordinate.isDouble() || !std::isfinite(number) || number < 0 || number > 1)
          return fail(error, "Invalid normalized stroke coordinate.");
      }
    }
  }
  return true;
}

void Store::apply(const Command& command, bool undo)
{
  const auto& remove = undo ? command.after : command.before;
  const auto& insert = undo ? command.before : command.after;
  if (remove) {
    for (int i = 0; i < items.size(); ++i)
      if (items[i].id() == remove->id()) {
        items.removeAt(i);
        break;
      }
  }
  if (insert) items.insert(qMin(command.index, int(items.size())), *insert);
  currentState = signature();
  ++version;
  emit changed();
}
bool Store::change(Command command)
{
  // Discard abandoned redo images and bound retained history without invalidating native tokens.
  QSet<QString> images;
  qint64 bytes = 0;
  auto count = [&](const std::optional<Record>& record) {
    if (record && !images.contains(record->id())) {
      images.insert(record->id());
      bytes += record->png.size();
    }
    if (record) bytes += QJsonDocument(record->metadata).toJson(QJsonDocument::Compact).size();
  };
  int retained = 0;
  for (auto it = commands.cbegin(); it != commands.cend() && it.key() <= historyPosition; ++it) {
    count(it->before);
    count(it->after);
    ++retained;
  }
  count(command.before);
  count(command.after);
  if (retained >= 8192 || bytes > 512LL * 1024 * 1024) return false;
  auto end = commands.upperBound(historyPosition);
  while (end != commands.end()) end = commands.erase(end);
  const int token = nextToken++;
  historyPosition = token;
  commands.insert(token, command);
  apply(command, false);
  emit commandAdded(token);
  return true;
}
bool Store::add(Record record, QString *error)
{
  if (!loadError.isEmpty()) return fail(error, loadError);
  if (items.size() >= MaxRecords || find(record.id()))
    return fail(error, "Annotation count or duplicate identity.");
  qint64 bytes = record.png.size();
  qint64 metadataBytes = QJsonDocument(record.metadata).toJson(QJsonDocument::Compact).size();
  for (const auto& item : items) {
    bytes += item.png.size();
    metadataBytes += QJsonDocument(item.metadata).toJson(QJsonDocument::Compact).size() + 1;
  }
  if (bytes + metadataBytes > MaxArchive - 2 * 1024 * 1024 || metadataBytes > 8 * 1024 * 1024 - 128)
    return fail(error, "Annotation document or manifest size limit exceeded.");
  if (!validate(record, error)) return false;
  if (!change({int(items.size()), {}, std::move(record)}))
    return fail(error, "Annotation undo history limit reached. Save and reopen the document.");
  return true;
}
bool Store::edit(const QString& id, const QString& comment)
{
  if (comment.size() > 65536 || !loadError.isEmpty()) return false;
  for (int i = 0; i < items.size(); ++i)
    if (items[i].id() == id) {
      if (items[i].metadata.value("comment") == comment) return true;
      Record after = items[i];
      after.metadata["comment"] = comment;
      const auto changedBytes = QJsonDocument(after.metadata).toJson(QJsonDocument::Compact).size();
      if (changedBytes > 1024 * 1024) return false;
      qint64 manifestBytes = changedBytes, total = 0;
      for (const auto& item : items) {
        if (item.id() != id)
          manifestBytes += QJsonDocument(item.metadata).toJson(QJsonDocument::Compact).size() + 1;
        total += item.png.size();
      }
      if (manifestBytes > 8 * 1024 * 1024 - 128 || total + manifestBytes > MaxArchive - 2 * 1024 * 1024)
        return false;
      return change({i, items[i], after});
    }
  return false;
}
bool Store::remove(const QString& id)
{
  if (!loadError.isEmpty()) return false;
  for (int i = 0; i < items.size(); ++i)
    if (items[i].id() == id) {
      return change({i, items[i], {}});
    }
  return false;
}
void Store::applyHistory(int token, bool undo)
{
  auto it = commands.constFind(token);
  if (it != commands.cend()) {
    apply(*it, undo);
    historyPosition = undo ? token - 1 : token;
  }
}
void Store::clearHistory()
{
  commands.clear();
  historyPosition = 0;
}

bool Store::load(const QString& companion, QString *error)
{
  path = companion;
  diskExisted = QFileInfo::exists(path);
  diskHash = diskExisted ? fileHash(path) : QByteArray();
  auto rejected = [&](const QString& message) {
    loadError = message;
    emit changed();
    return fail(error, message);
  };
  QVector<Record> loaded;
  if (diskExisted) {
    QFile file(path);
    if (file.size() > MaxArchive || !file.open(QIODevice::ReadOnly))
      return rejected("Cannot read annotation companion (size/access).");
    QByteArray bytes = file.read(MaxArchive + 1);
    zip_error_t ze;
    zip_error_init(&ze);
    auto source = zip_source_buffer_create(bytes.constData(), bytes.size(), 0, &ze);
    auto archive = source ? zip_open_from_source(source, ZIP_RDONLY, &ze) : nullptr;
    if (!archive) {
      if (source) zip_source_free(source);
      zip_error_fini(&ze);
      return rejected("Corrupt annotation archive; original file preserved.");
    }
    zip_error_fini(&ze);
    auto rejectArchive = [&](const QString& message) {
      zip_discard(archive);
      return rejected(message);
    };
    const auto count = zip_get_num_entries(archive, 0);
    if (count < 1 || count > MaxRecords + 1)
      return rejectArchive("Invalid annotation archive entry count.");
    QByteArray manifest;
    auto index = zip_name_locate(archive, "manifest.json", 0);
    if (index < 0 || !readEntry(archive, index, 8 * 1024 * 1024, manifest))
      return rejectArchive("Missing/invalid annotation manifest.");
    QJsonParseError parse;
    auto object = QJsonDocument::fromJson(manifest, &parse).object();
    if (parse.error != QJsonParseError::NoError || object.value("schema") != "openscad.annotations" ||
        object.value("version").toInt() != 1 || !object.value("records").isArray())
      return rejectArchive("Unsupported or corrupt annotation manifest; original file preserved.");
    QSet<QString> names{"manifest.json"}, ids;
    qint64 total = manifest.size();
    for (const auto& value : object.value("records").toArray()) {
      Record record{value.toObject(), {}};
      const QString entry = record.id() + ".png";
      if (ids.contains(record.id()) || QUuid(record.id()).isNull())
        return rejectArchive("Duplicate/invalid annotation ID.");
      ids.insert(record.id());
      names.insert(entry);
      index = zip_name_locate(archive, entry.toUtf8().constData(), 0);
      if (index < 0 || !readEntry(archive, index, MaxPng, record.png))
        return rejectArchive("Missing/oversized annotation PNG.");
      total += record.png.size();
      QString validation;
      if (total > MaxArchive || !validate(record, &validation))
        return rejectArchive("Invalid annotation: " + validation);
      loaded.push_back(std::move(record));
    }
    if (count != names.size()) return rejectArchive("Unexpected archive entries.");
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i) {
      const char *name = zip_get_name(archive, i, 0);
      if (!name || !names.remove(QString::fromUtf8(name)))
        return rejectArchive("Unexpected/duplicate archive entry.");
    }
    zip_discard(archive);
  }
  items = std::move(loaded);
  currentState = savedState = signature();
  loadError.clear();
  clearHistory();
  ++version;
  emit changed();
  return true;
}

bool Store::canSave(const QString& companion, QString *error, bool replaceDestination) const
{
  if (!loadError.isEmpty()) return fail(error, "Unreadable companion preserved: " + loadError);
  const bool exists = QFileInfo::exists(companion);
  if (companion == path) {
    if (exists != diskExisted || (exists && fileHash(companion) != diskHash))
      return fail(error,
                  "Annotations changed on disk. Save to a new filename to preserve both versions.");
  } else if (exists && !replaceDestination) {
    return fail(error,
                "Destination already has annotations. Confirm replacement or choose another filename.");
  }
  return true;
}
bool Store::save(const QString& companion, bool adopt, QString *error, bool replaceDestination)
{
  QLockFile lock(companion + ".lock");
  if (!lock.tryLock(0))
    return fail(error, "Annotation companion is locked or its directory is not writable.");
  if (!canSave(companion, error, replaceDestination)) return false;
  // Avoid creating empty companions for untouched documents; do overwrite a saved empty store.
  if (items.isEmpty() && !dirty() && !QFileInfo::exists(companion) && !diskExisted) {
    if (adopt) {
      path = companion;
      diskHash.clear();
      diskExisted = false;
    }
    return true;
  }
  QJsonArray records;
  for (const auto& item : items) records.append(item.metadata);
  QByteArray manifest =
    QJsonDocument(QJsonObject{{"schema", "openscad.annotations"}, {"version", 1}, {"records", records}})
      .toJson(QJsonDocument::Compact);
  if (manifest.size() > 8 * 1024 * 1024) return fail(error, "Annotation manifest exceeds 8 MiB.");
  zip_error_t ze;
  zip_error_init(&ze);
  auto source = zip_source_buffer_create(nullptr, 0, 0, &ze);
  auto archive = source ? zip_open_from_source(source, ZIP_CREATE | ZIP_TRUNCATE, &ze) : nullptr;
  zip_error_fini(&ze);
  if (!archive) {
    if (source) zip_source_free(source);
    return fail(error, "Cannot create annotation archive.");
  }
  zip_source_keep(source);
  auto put = [&](const QByteArray& name, const QByteArray& bytes) {
    auto entry = zip_source_buffer(archive, bytes.constData(), bytes.size(), 0);
    if (!entry) return false;
    auto index = zip_file_add(archive, name.constData(), entry, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
      zip_source_free(entry);
      return false;
    }
    return zip_set_file_compression(archive, index, ZIP_CM_STORE, 0) == 0;
  };
  bool ok = put("manifest.json", manifest);
  for (const auto& item : items)
    if (ok) ok = put((item.id() + ".png").toUtf8(), item.png);
  if (ok && zip_close(archive) == 0) archive = nullptr;
  else ok = false;
  if (archive) zip_discard(archive);
  zip_stat_t stat;
  zip_stat_init(&stat);
  QByteArray bytes;
  if (ok)
    ok = zip_source_stat(source, &stat) == 0 && stat.size <= MaxArchive && zip_source_open(source) == 0;
  if (ok) {
    bytes.resize(static_cast<int>(stat.size));
    qint64 offset = 0;
    while (offset < bytes.size()) {
      auto count = zip_source_read(source, bytes.data() + offset, bytes.size() - offset);
      if (count <= 0) {
        ok = false;
        break;
      }
      offset += count;
    }
    zip_source_close(source);
  }
  zip_source_free(source);
  if (!ok) return fail(error, "Cannot serialize annotation archive within limits.");
  QSaveFile file(companion);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !canSave(companion, error, replaceDestination) || !file.commit())
    return fail(error, error && !error->isEmpty()
                         ? *error
                         : "Cannot publish annotation companion: " + file.errorString());
  if (adopt) {
    path = companion;
    diskExisted = true;
    diskHash = hash(bytes);
    savedState = signature();
    emit changed();
  }
  return true;
}
}  // namespace AgentAnnotations
