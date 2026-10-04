#pragma once
#include "glview/CameraPose.h"
#include "core/design/DesignObjects.h"
#include <QObject>
#include <QVector>
#include <functional>

class EditorInterface;

class SavedViews : public QObject {
  Q_OBJECT
public:
  enum class Backing { PersonalInspection };
  struct Entry {
    std::shared_ptr<const Design::Record> record;
    Backing backing = Backing::PersonalInspection;
    QString id() const { return QString::fromStdString(record->id); }
    QString name() const { return QString::fromStdString(record->label); }
    CameraPose pose() const;
  };
  explicit SavedViews(QObject *parent = nullptr);
  static SavedViews *forEditor(EditorInterface *editor);
  const QVector<Entry>& entries() const { return records; }
  bool dirty() const { return modified; }
  quint64 revision() const { return generation; }
  QString problem() const { return loadError; }
  QString add(const CameraPose& pose);
  void rename(const QString& id, const QString& name);
  void remove(const QString& id);
  bool restore(const Entry&);
  const Entry *find(const QString& id) const;
  bool load(const QString& source);
  // Hold the companion lock across both writes. Source and metadata commits remain independent.
  bool save(const QString& source, bool copy, const std::function<bool()>& writeSource, QString& error);
  bool backup(const QString& source, QString& error) const;
  QByteArray serialize() const;
  static QString companion(const QString& source) { return source + ".views.json"; }
signals:
  void changed();
private:
  QVector<Entry> records;
  quint64 generation = 0;
  QString identity, loadError;
  QByteArray fingerprint;
  bool modified=false, existed=false;
  void touch();
};
