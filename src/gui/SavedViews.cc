#include "gui/SavedViews.h"
#include "gui/Editor.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <limits>

namespace {
constexpr qint64 maxBytes=4*1024*1024;
constexpr int maxEntries=1000, maxName=256;
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256); }
QString canonical(const QString& path) { return path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath(); }
QJsonArray vector(const Eigen::Vector3d& v) { return {v.x(),v.y(),v.z()}; }
bool vector(const QJsonValue& value,Eigen::Vector3d& v) {
  const auto a=value.toArray();
  if (a.size()!=3) return false;
  for (int i=0;i<3;++i) { if (!a[i].isDouble() || !std::isfinite(a[i].toDouble())) return false; v[i]=a[i].toDouble(); }
  return true;
}
bool atomicWrite(const QString& path,const QByteArray& data,QString& error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit()) {
    error=QObject::tr("Cannot save views to %1: %2").arg(path,file.errorString()); return false;
  }
  return true;
}
}

SavedViews::SavedViews(QObject *parent) : QObject(parent) {}
SavedViews *SavedViews::forEditor(EditorInterface *editor) {
  auto store=editor->findChild<SavedViews *>(QString(),Qt::FindDirectChildrenOnly);
  if (!store) {
    store=new SavedViews(editor);
    store->load(editor->filepath);
    connect(store,&SavedViews::changed,editor,[editor]{emit editor->modificationChanged(editor);});
  }
  return store;
}
bool EditorInterface::hasViewChanges() const {
  auto store=findChild<SavedViews *>(QString(),Qt::FindDirectChildrenOnly);
  return store && store->dirty();
}
void SavedViews::touch() { modified=true; emit changed(); }
const SavedViews::Entry *SavedViews::find(const QString& id) const {
  for (const auto& e:records) if (e.id==id) return &e;
  return nullptr;
}
QString SavedViews::add(const CameraPose& pose) {
  if (!loadError.isEmpty() || !pose.valid() || records.size()>=maxEntries) return {};
  QSet<QString> names; for (const auto& e:records) names.insert(e.name);
  int n=1; while (names.contains(tr("View %1").arg(n))) ++n;
  const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
  records.append({id,tr("View %1").arg(n),pose}); touch(); return id;
}
void SavedViews::rename(const QString& id,const QString& name) {
  const auto trimmed=name.trimmed().left(maxName);
  if (trimmed.isEmpty()) return;
  for (auto& e:records) if (e.id==id && e.name!=trimmed) { e.name=trimmed; touch(); return; }
}
void SavedViews::remove(const QString& id) {
  for (int i=0;i<records.size();++i) if (records[i].id==id) { records.removeAt(i); touch(); return; }
}
QByteArray SavedViews::serialize() const {
  QJsonArray views;
  for (const auto& e:records) views.append(QJsonObject{{"id",e.id},{"name",e.name},
    {"rotation",vector(e.camera.rotation)},{"translation",vector(e.camera.translation)},
    {"distance",e.camera.distance},{"fov",e.camera.fov},
    {"projection",e.camera.projection==Camera::ProjectionType::ORTHOGONAL ? "orthographic" : "perspective"}});
  return QJsonDocument(QJsonObject{{"version",1},{"document",identity},{"views",views}}).toJson();
}
bool SavedViews::load(const QString& source) {
  identity=canonical(source); records.clear(); modified=false; loadError.clear(); fingerprint.clear(); existed=false;
  if (source.isEmpty()) return true;
  QFile file(companion(identity)); existed=file.exists();
  if (!existed) return true;
  auto fail=[&] { loadError=tr("Saved views could not be loaded from %1. The file has been preserved; use a different Save As destination to recover new work.").arg(file.fileName()); return false; };
  if (!file.open(QIODevice::ReadOnly) || file.size()>maxBytes) return fail();
  auto bytes=file.readAll(); if (file.error()!=QFile::NoError) return fail();
  fingerprint=hash(bytes);
  QJsonParseError error;
  const auto doc=QJsonDocument::fromJson(bytes,&error);
  const auto root=doc.object();
  if (error.error!=QJsonParseError::NoError || root.value("version").toDouble()!=1 || !root.value("views").isArray()) return fail();
  const auto list=root.value("views").toArray(); if (list.size()>maxEntries) return fail();
  QVector<Entry> parsed; QSet<QString> ids;
  for (const auto& value:list) {
    const auto object=value.toObject(); Entry e;
    e.id=object.value("id").toString(); e.name=object.value("name").toString();
    const auto projection=object.value("projection").toString();
    e.camera.distance=object.value("distance").toDouble(std::numeric_limits<double>::quiet_NaN());
    e.camera.fov=object.value("fov").toDouble(std::numeric_limits<double>::quiet_NaN());
    e.camera.projection=projection=="orthographic" ? Camera::ProjectionType::ORTHOGONAL : Camera::ProjectionType::PERSPECTIVE;
    if (QUuid(e.id).isNull() || ids.contains(e.id) || e.name.trimmed().isEmpty() || e.name.size()>maxName ||
        !vector(object.value("rotation"),e.camera.rotation) || !vector(object.value("translation"),e.camera.translation) ||
        (projection!="orthographic" && projection!="perspective") || !e.camera.valid()) return fail();
    ids.insert(e.id); parsed.append(e);
  }
  records=parsed; emit changed(); return true;
}
bool SavedViews::save(const QString& source,bool copy,const std::function<bool()>& writeSource,QString& error) {
  const auto destination=canonical(source);
  const bool same=destination==identity;
  if (!same && QFileInfo::exists(companion(destination))) {
    error=tr("The destination already has saved views: %1. Choose another filename to preserve both documents.").arg(companion(destination));
    return false;
  }
  const bool needsWrite=modified || (!same && (existed || !records.isEmpty()));
  if (!needsWrite) return writeSource();
  if (!loadError.isEmpty()) { error=loadError; return false; }
  const auto path=companion(destination);
  QLockFile lock(path+".lock");
  if (!lock.tryLock(0)) { error=tr("Saved views are being written by another window: %1. Retry after that save finishes.").arg(path); return false; }
  QFile existing(path);
  const bool present=existing.exists();
  bool matches=present==(same && existed);
  if (present) {
    matches=matches && existing.open(QIODevice::ReadOnly) && existing.size()<=maxBytes;
    if (matches) { auto bytes=existing.readAll(); matches=existing.error()==QFile::NoError && hash(bytes)==fingerprint; }
    existing.close();
  }
  if (!matches) { error=tr("Saved views changed on disk or the destination already has views: %1. Your in-memory views are retained. Save As to a new filename to preserve both versions.").arg(path); return false; }
  if (!writeSource()) return false;
  if (!copy) identity=destination;
  if (!atomicWrite(path,serialize(),error)) {
    if (!copy) { modified=true; existed=present; emit changed(); }
    error=tr("The source was saved, but the saved views were not. ")+error; return false;
  }
  if (!copy) { fingerprint=hash(serialize()); existed=true; modified=false; emit changed(); }
  return true;
}
bool SavedViews::backup(const QString& source,QString& error) const {
  if (!modified && records.isEmpty()) return true;
  return atomicWrite(companion(source),serialize(),error);
}
