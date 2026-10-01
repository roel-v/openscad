#pragma once
#include "AnnotationStore.h"
#include <QJsonObject>
#include <QHash>
#include <QTcpServer>
#include <QTimer>
class QTcpSocket;

namespace AgentAnnotations {
struct Snapshot {
  QJsonObject status;
  QHash<QString, QVector<Record>> records;
};
// Owns only detached evidence and transport state; never touches widgets or OpenGL.
class Transport : public QObject
{
  Q_OBJECT
public:
  explicit Transport(QObject *parent = nullptr);
  ~Transport() override { stop(); }
  void start();
  void stop();
  void publish(Snapshot snapshot);
  void complete(const QString& serverId, const QString& requestId, QJsonObject result, QByteArray png,
                QString error);
  QJsonObject connection() const { return info; }
  bool activeJob(const QString& server, const QString& id) const
  {
    return listener && info.value("server_id") == server && jobs.contains(id) &&
           jobs[id].value.value("state") == "queued";
  }
signals:
  void listening(QJsonObject info, QString error);
  void operation(QString serverId, QString requestId, QString path, QJsonObject body);

private:
  QTcpServer *listener = nullptr;
  QTimer *expiry = nullptr;
  QJsonObject info;
  Snapshot snapshot;
  qint64 heartbeat = 0;
  struct Job {
    QByteArray input;
    QJsonObject value;
    qint64 created;
    qint64 bytes = 0;
  };
  QHash<QString, Job> jobs;
  int connections = 0;
  qint64 responseBytes = 0;
  QString discovery;
  void accept();
  void route(QTcpSocket *socket, const QByteArray& header, const QByteArray& body);
  void respond(QTcpSocket *socket, int code, QJsonObject value);
  void expire();
};
}  // namespace AgentAnnotations
