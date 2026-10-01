#pragma once
#include "AnnotationStore.h"
#include <QJsonObject>
#include <QHash>
#include <QTcpServer>
#include <QTimer>
#include <functional>
class QTcpSocket;

namespace AgentAnnotations {
struct Snapshot {
  QJsonObject status;
  QHash<QString, QVector<Record>> records;
  QHash<QString, QJsonObject> reads;
};
// Owns only detached evidence and transport state; never touches widgets or OpenGL.
class Transport : public QObject
{
  Q_OBJECT
public:
  explicit Transport(QObject *parent = nullptr, std::function<qint64()> clock = {});
  ~Transport() override { stop(); }
  void start(bool broadAccess = false);
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
  bool beginJob(const QString& server, const QString& id);
signals:
  void listening(QJsonObject info, QString error);
  void operation(QString serverId, QString requestId, QString path, QJsonObject body);

private:
  QTcpServer *listener = nullptr;
  QTimer *expiry = nullptr;
  QJsonObject info;
  Snapshot snapshot;
  qint64 heartbeat = 0;
  quint64 stateGeneration = 0;
  bool broadAccess = false;
  std::function<qint64()> clock;
  qint64 now() const;
  struct Job {
    QByteArray input;
    QJsonObject value;
    qint64 created;
    qint64 bytes = 0;
    qint64 deadline = 0;
  };
  QHash<QString, Job> jobs;
  int connections = 0;
  qint64 responseBytes = 0;
  QString discovery;
  void accept();
  void route(QTcpSocket *socket, const QByteArray& header, const QByteArray& body);
  void respond(QTcpSocket *socket, int code, QJsonObject value);
  void expire();
  void routeV2(QTcpSocket *socket, const QByteArray& method, const QString& path,
               const QByteArray& bytes);
  void enqueueV2(QTcpSocket *socket, const QString& path, const QJsonObject& body);
};
}  // namespace AgentAnnotations
