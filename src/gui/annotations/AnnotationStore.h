#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QVector>
#include <optional>

namespace AgentAnnotations {

struct Record {
  QJsonObject metadata;
  QByteArray png;
  QString id() const { return metadata.value("id").toString(); }
};

// All mutations are on the GUI thread. QByteArray copies share immutable PNG storage.
class Store : public QObject
{
  Q_OBJECT
public:
  static constexpr qint64 MaxPng = 32 * 1024 * 1024;
  static constexpr qint64 MaxArchive = 256 * 1024 * 1024;
  static constexpr int MaxRecords = 1024;
  explicit Store(QObject *parent = nullptr);
  QString sessionId;
  void renewSession();
  const QVector<Record>& records() const { return items; }
  const Record *find(const QString& id) const;
  bool dirty() const;
  void markUnsaved()
  {
    savedState.clear();
    emit changed();
  }
  QString diagnostic() const { return loadError; }
  quint64 revision() const { return version; }
  bool add(Record record, QString *error = nullptr);
  bool edit(const QString& id, const QString& comment);
  bool remove(const QString& id);
  void applyHistory(int token, bool undo);
  void clearHistory();
  bool load(const QString& companion, QString *error = nullptr);
  // adopt=false is Save a Copy/backup: do not change path, saved state or history.
  bool save(const QString& companion, bool adopt, QString *error = nullptr,
            bool replaceDestination = false);
  bool canSave(const QString& companion, QString *error = nullptr,
               bool replaceDestination = false) const;
  static bool validate(const Record& record, QString *error = nullptr);
  static QByteArray hash(const QByteArray& bytes);

signals:
  void sessionChanged();
  void changed();
  void commandAdded(int token);
  void historyCleared();

private:
  struct Command {
    int index;
    std::optional<Record> before, after;
  };
  QVector<Record> items;
  QMap<int, Command> commands;
  int nextToken = 1;
  int historyPosition = 0;
  quint64 version = 0;
  QByteArray savedState, currentState;
  QString path, loadError;
  QByteArray diskHash;
  bool diskExisted = false;
  QByteArray signature() const;
  bool change(Command command);
  void apply(const Command& command, bool undo);
};
}  // namespace AgentAnnotations
