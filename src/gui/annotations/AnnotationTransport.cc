#include "AnnotationTransport.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

namespace AgentAnnotations {
namespace {
constexpr int MaxBody = 256 * 1024, MaxHeader = 16 * 1024;
qint64 now()
{
  return QDateTime::currentMSecsSinceEpoch();
}
bool validId(const QString& id)
{
  return !QUuid(id).isNull();
}
}  // namespace
Transport::Transport(QObject *parent) : QObject(parent)
{
}
void Transport::start()
{
  if (listener) return;
  listener = new QTcpServer(this);
  if (!listener->listen(QHostAddress::LocalHost, 0)) {
    auto error = listener->errorString();
    delete listener;
    listener = nullptr;
    emit listening({}, error);
    return;
  }
  connect(listener, &QTcpServer::newConnection, this, &Transport::accept);
  const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString url = "http://127.0.0.1:" + QString::number(listener->serverPort());
  info = {{"product", "OpenSCAD"},
          {"protocol", 1},
          {"schema", 1},
          {"server_id", id},
          {"url", url},
          {"pid", QString::number(QCoreApplication::applicationPid())},
          {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
          {"executable", QCoreApplication::applicationFilePath()},
          {"capabilities", QJsonArray{"annotations.read", "viewport.capture", "camera.set",
                                      "camera.restore", "status.read"}}};
  QString directory =
    QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/AgentAnnotations/OpenSCAD";
  if (!QDir().mkpath(directory)) {
    stop();
    emit listening({}, "Cannot create annotation discovery directory.");
    return;
  }
  discovery = directory + "/" + id + ".json";
  QSaveFile file(discovery);
  QByteArray data = QJsonDocument(info).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
    stop();
    emit listening({}, "Cannot publish annotation discovery.");
    return;
  }
  expiry = new QTimer(this);
  expiry->setInterval(1000);
  connect(expiry, &QTimer::timeout, this, &Transport::expire);
  expiry->start();
  emit listening(info, {});
}
void Transport::stop()
{
  if (expiry) {
    delete expiry;
    expiry = nullptr;
  }
  if (listener) {
    listener->close();
    delete listener;
    listener = nullptr;
  }
  for (auto socket : findChildren<QTcpSocket *>()) socket->abort();
  if (!discovery.isEmpty()) {
    QFile::remove(discovery);
    discovery.clear();
  }
  jobs.clear();
  responseBytes = 0;
  info = {};
  snapshot = {};
  heartbeat = 0;
  emit listening({}, {});
}
void Transport::publish(Snapshot value)
{
  snapshot = std::move(value);
  heartbeat = now();
}
void Transport::respond(QTcpSocket *socket, int code, QJsonObject value)
{
  if (socket->state() == QAbstractSocket::UnconnectedState) return;
  const QByteArray data = QJsonDocument(value).toJson(QJsonDocument::Compact);
  QByteArray response = "HTTP/1.1 " + QByteArray::number(code) + " " + (code < 300 ? "OK" : "Error") +
                        "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: "
                        "close\r\nContent-Length: " +
                        QByteArray::number(data.size()) + "\r\n\r\n";
  socket->write(response);
  socket->write(data);
  socket->disconnectFromHost();
}
void Transport::accept()
{
  while (listener && listener->hasPendingConnections()) {
    auto socket = listener->nextPendingConnection();
    socket->setParent(this);
    if (connections >= 16) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
    ++connections;
    socket->setReadBufferSize(MaxHeader + MaxBody + 1);
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
      --connections;
      socket->deleteLater();
    });
    auto deadline = new QTimer(socket);
    deadline->setSingleShot(true);
    deadline->start(5000);
    connect(deadline, &QTimer::timeout, socket, &QTcpSocket::abort);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, deadline] {
      if (socket->property("handled").toBool()) return;
      QByteArray data = socket->property("request").toByteArray() + socket->readAll();
      int split = data.indexOf("\r\n\r\n");
      if (data.size() > MaxHeader + MaxBody || split > MaxHeader ||
          (split < 0 && data.size() > MaxHeader)) {
        socket->setProperty("handled", true);
        respond(socket, 413, {{"error", "Request limit exceeded."}});
        return;
      }
      if (split < 0) {
        socket->setProperty("request", data);
        return;
      }
      qint64 length = 0;
      bool found = false;
      for (auto line : data.left(split).split('\n')) {
        if (line.toLower().startsWith("content-length:")) {
          bool ok = false;
          auto number = line.mid(15).trimmed().toLongLong(&ok);
          if (found || !ok || number < 0 || number > MaxBody) {
            socket->setProperty("handled", true);
            respond(socket, 400, {{"error", "Invalid Content-Length."}});
            return;
          }
          found = true;
          length = number;
        }
      }
      if (data.size() < split + 4 + length) {
        socket->setProperty("request", data);
        return;
      }
      socket->setProperty("handled", true);
      socket->setProperty("request", QByteArray());
      deadline->start(10000);  // Also bound a client that stops reading its response.
      if (data.size() != split + 4 + length) {
        respond(socket, 400, {{"error", "Pipelining is unsupported."}});
        return;
      }
      route(socket, data.left(split), data.mid(split + 4));
    });
  }
}
void Transport::route(QTcpSocket *socket, const QByteArray& header, const QByteArray& bytes)
{
  if (!listener) {
    respond(socket, 503, {{"error", "Stopped."}});
    return;
  }
  const auto lines = header.split('\n');
  const auto request = lines.first().trimmed().split(' ');
  if (request.size() != 3 || request[2] != "HTTP/1.1" || !request[1].startsWith('/') ||
      request[1].startsWith("//")) {
    respond(socket, 400, {{"error", "Invalid request line."}});
    return;
  }
  QHash<QByteArray, QByteArray> fields;
  for (int i = 1; i < lines.size(); ++i) {
    auto line = lines[i].trimmed();
    int colon = line.indexOf(':');
    if (colon < 1 || fields.contains(line.left(colon).toLower())) {
      respond(socket, 400, {{"error", "Malformed or duplicate header."}});
      return;
    }
    fields.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
  }
  const auto host = "127.0.0.1:" + QByteArray::number(listener->serverPort());
  if (fields.value("host") != host || fields.contains("origin") ||
      fields.contains("transfer-encoding") || fields.value("x-agent-annotations") != "1" ||
      fields.value("x-agent-annotations-server") != info.value("server_id").toString().toUtf8()) {
    respond(
      socket, 403,
      {{"error", "Numeric loopback Host, protocol/server headers and no browser Origin are required."}});
    return;
  }
  QUrl url = QUrl::fromEncoded(request[1]);
  QUrlQuery query(url);
  QString path = url.path();
  if (request[0] == "GET") {
    if (!bytes.isEmpty()) {
      respond(socket, 400, {{"error", "GET body is unsupported."}});
      return;
    }
    if (path == "/v1/info") {
      auto value = info;
      value["gui_heartbeat_age_ms"] = heartbeat ? double(now() - heartbeat) : -1;
      value["gui_responsive"] = heartbeat && now() - heartbeat < 5000;
      respond(socket, 200, value);
      return;
    }
    if (path == "/v1/documents" || path == "/v1/viewports") {
      QString key = path.mid(4);
      respond(socket, 200,
              {{key, snapshot.status.value(key)},
               {"gui_heartbeat_age_ms", heartbeat ? double(now() - heartbeat) : -1}});
      return;
    }
    if (path.startsWith("/v1/requests/")) {
      auto id = path.mid(13);
      auto it = jobs.constFind(id);
      if (it == jobs.cend())
        respond(socket, 410,
                {{"state", "expired"},
                 {"error", "Unknown or expired request; do not silently submit a new ID."}});
      else respond(socket, 200, it->value);
      return;
    }
    if (path == "/v1/annotations" || path.startsWith("/v1/annotations/")) {
      const auto document = query.queryItemValue("document_id");
      auto it = snapshot.records.constFind(document);
      if (it == snapshot.records.cend()) {
        respond(socket, 404, {{"error", "Unknown document session."}});
        return;
      }
      const QString annotation = path == "/v1/annotations" ? QString() : path.mid(16);
      if (!annotation.isEmpty()) {
        for (const auto& record : *it)
          if (record.id() == annotation) {
            auto result = record.metadata;
            result["png_base64"] = QString::fromLatin1(record.png.toBase64());
            respond(socket, 200, result);
            return;
          }
        respond(socket, 404, {{"error", "Unknown annotation."}});
        return;
      }
      QJsonArray records;
      auto text = query.queryItemValue("q");
      for (auto rit = it->crbegin(); rit != it->crend(); ++rit)
        if (rit->metadata.value("comment").toString().contains(text, Qt::CaseInsensitive))
          records.append(rit->metadata);
      respond(socket, 200, {{"document_id", document}, {"annotations", records}});
      return;
    }
    respond(socket, 404, {{"error", "Unknown read route."}});
    return;
  }
  const bool capture = path == "/v1/capture";
  const bool restore =
    QRegularExpression("^/v1/annotations/[a-fA-F0-9-]{36}/restore$").match(path).hasMatch();
  const bool view = QRegularExpression("^/v1/viewports/[a-fA-F0-9-]{36}/view$").match(path).hasMatch();
  if (request[0] != "POST" || (!capture && !restore && !view)) {
    respond(socket, 404, {{"error", "Unsupported operation."}});
    return;
  }
  if (!fields.value("content-type").startsWith("application/json")) {
    respond(socket, 415, {{"error", "JSON required."}});
    return;
  }
  QJsonParseError parse;
  auto document = QJsonDocument::fromJson(bytes, &parse);
  auto body = document.object();
  if (parse.error != QJsonParseError::NoError || !document.isObject() ||
      body.value("server_id") != info.value("server_id") ||
      !validId(body.value("request_id").toString()) || !validId(body.value("document_id").toString()) ||
      !validId(body.value("window_id").toString()) || !validId(body.value("viewport_id").toString()) ||
      !body.value("expected_result_id").isString()) {
    respond(
      socket, 400,
      {{"error",
        "Explicit server, request, document, window, viewport and expected result IDs are required."}});
    return;
  }
  expire();
  const auto id = body.value("request_id").toString();
  const QByteArray input = path.toUtf8() + '\n' + QJsonDocument(body).toJson(QJsonDocument::Compact);
  auto existing = jobs.constFind(id);
  if (existing != jobs.cend()) {
    if (existing->input != input)
      respond(socket, 409, {{"error", "Request ID reused with different input."}});
    else respond(socket, 200, existing->value);
    return;
  }
  if (!heartbeat || now() - heartbeat >= 5000) {
    respond(socket, 503,
            {{"error", "GUI heartbeat is stale; retry the same request after it becomes responsive."}});
    return;
  }
  int pending = 0;
  for (const auto& job : jobs)
    if (job.value.value("state") == "queued") ++pending;
  if (pending >= 32 || jobs.size() >= 128 || responseBytes >= 128LL * 1024 * 1024) {
    respond(socket, 429, {{"error", "Request/result capacity reached; wait for expiry."}});
    return;
  }
  bool targetFound = false;
  for (const auto& value : snapshot.status.value("viewports").toArray()) {
    const auto viewport = value.toObject();
    if (viewport.value("viewport_id") != body.value("viewport_id") ||
        viewport.value("window_id") != body.value("window_id"))
      continue;
    targetFound = true;
    if (viewport.value("document_id") != body.value("document_id") ||
        viewport.value("result").toObject().value("id").toString() !=
          body.value("expected_result_id").toString()) {
      respond(socket, 409, {{"error", "Stale document or result."}});
      return;
    }
    if (viewport.value("busy").toBool()) {
      respond(socket, 409, {{"error", "Viewport is busy."}});
      return;
    }
  }
  if (!targetFound) {
    respond(socket, 404, {{"error", "Unknown viewport."}});
    return;
  }
  QJsonObject value{{"request_id", id}, {"state", "queued"}};
  jobs.insert(id, {input, value, now(), 0});
  respond(socket, 202, value);
  emit operation(info.value("server_id").toString(), id, path, body);
}
void Transport::complete(const QString& serverId, const QString& id, QJsonObject result, QByteArray png,
                         QString error)
{
  if (!listener || info.value("server_id") != serverId || !jobs.contains(id)) return;
  auto& job = jobs[id];
  if (job.value.value("state") != "queued") return;
  if (png.size() > Store::MaxPng || responseBytes + (png.size() * 4 / 3) > 128LL * 1024 * 1024) {
    error = "Response image budget exceeded.";
    png.clear();
  }
  job.value["state"] = error.isEmpty() ? "succeeded" : "failed";
  if (error.isEmpty()) {
    if (!png.isEmpty()) result["png_base64"] = QString::fromLatin1(png.toBase64());
    job.value["result"] = result;
  } else job.value["error"] = error;
  job.bytes = QJsonDocument(job.value).toJson(QJsonDocument::Compact).size();
  if (responseBytes + job.bytes > 128LL * 1024 * 1024) {
    job.value = {{"request_id", id}, {"state", "failed"}, {"error", "Response budget exceeded."}};
    job.bytes = QJsonDocument(job.value).toJson(QJsonDocument::Compact).size();
  }
  responseBytes += job.bytes;
}
void Transport::expire()
{
  for (auto it = jobs.begin(); it != jobs.end();) {
    if (now() - it->created > 300000) {
      responseBytes -= it->bytes;
      it = jobs.erase(it);
    } else ++it;
  }
}
}  // namespace AgentAnnotations
