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
#include <QCryptographicHash>
#include <chrono>

namespace AgentAnnotations {
namespace {
constexpr int MaxBody = 256 * 1024, MaxHeader = 16 * 1024;
bool validId(const QString& id)
{
  return !QUuid(id).isNull();
}
}  // namespace
Transport::Transport(QObject *parent, std::function<qint64()> clock) : QObject(parent), clock(std::move(clock))
{
}
qint64 Transport::now() const
{
  if (clock) return clock();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Transport::start(bool broad)
{
  if (listener) return;
  broadAccess = broad;
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
  info["agent_api_versions"] = broadAccess ? QJsonArray{2} : QJsonArray{};
  info["access_mode"] = broadAccess ? "agent" : "annotations";
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
  if (snapshot.status != value.status) ++stateGeneration;
  snapshot = std::move(value);
  heartbeat = now();
}
void Transport::respond(QTcpSocket *socket, int code, QJsonObject value)
{
  if (socket->state() == QAbstractSocket::UnconnectedState) return;
  // The original client polls only "queued". Preserve its pending-state contract.
  if (!socket->property("agentV2").toBool() && value.value("state") == "running")
    value["state"] = "queued";
  if (socket->property("agentV2").toBool() && code >= 400 && !value.contains("code")) {
    value["code"] = code == 403 ? "transport_guard" : code == 410 ? "expired" : "invalid_request";
    value["message"] = value.value("error").toString("Request failed.");
  }
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
  const bool v2 = request[1].startsWith("/v2/");
  socket->setProperty("agentV2", v2);
  const bool discoveryRequest = v2 && request[0] == "GET" && request[1] == "/v2/info";
  const auto protocolHeader = v2 ? "x-openscad-agent" : "x-agent-annotations";
  const auto serverHeader = v2 ? "x-openscad-agent-server" : "x-agent-annotations-server";
  bool browser = fields.contains("origin");
  for (auto it = fields.cbegin(); it != fields.cend(); ++it)
    browser = browser || it.key().startsWith("sec-fetch-");
  const auto host = "127.0.0.1:" + QByteArray::number(listener->serverPort());
  if (fields.value("host") != host || browser ||
      fields.contains("transfer-encoding") || fields.value(protocolHeader) != "1" ||
      (!discoveryRequest && fields.value(serverHeader) != info.value("server_id").toString().toUtf8())) {
    respond(
      socket, 403,
      {{"error", "Numeric loopback Host, protocol/server headers and no browser Origin are required."}});
    return;
  }
  QUrl url = QUrl::fromEncoded(request[1]);
  QUrlQuery query(url);
  QString path = url.path();
  if (v2) {
    if (!broadAccess) {
      respond(socket, 403, {{"code", "access_disabled"}, {"message", "Enable broad agent access first."}});
      return;
    }
    if (request[0] == "POST" && fields.value("content-type").split(';').first().trimmed() != "application/json") {
      respond(socket, 415, {{"code", "invalid_media_type"}, {"message", "JSON required."}});
      return;
    }
    routeV2(socket, request[0], path, bytes);
    return;
  }
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
  const QByteArray input = QCryptographicHash::hash(
    path.toUtf8() + '\n' + QJsonDocument(body).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
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
  int pending = 0, retained = 0;
  for (const auto& job : jobs)
    if (job.value.value("state") == "queued" || job.value.value("state") == "running") ++pending;
  for (const auto& job : jobs)
    if (job.value.value("state") != "expired") ++retained;
  if (pending >= 32 || retained >= 128 || jobs.size() >= 10000 || responseBytes >= 128LL * 1024 * 1024) {
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
  QJsonObject value{{"request_id", id}, {"state", "queued"}, {"execution_started", false},
                   {"document_id", body.value("document_id")}, {"server_id", info.value("server_id")}};
  jobs.insert(id, {input, value, now(), 0, now() + 10000});
  respond(socket, 202, value);
  emit operation(info.value("server_id").toString(), id, path, body);
}
void Transport::complete(const QString& serverId, const QString& id, QJsonObject result, QByteArray png,
                         QString error)
{
  if (!listener || info.value("server_id") != serverId || !jobs.contains(id)) return;
  auto& job = jobs[id];
  if (job.value.value("state") != "queued" && job.value.value("state") != "running") return;
  if (png.size() > Store::MaxPng || responseBytes + (png.size() * 4 / 3) > 128LL * 1024 * 1024) {
    error = "Response image budget exceeded.";
    png.clear();
  }
  job.value["state"] = error.isEmpty() ? "succeeded" : "failed";
  job.created = now();  // Retain terminal outcomes for five minutes after completion.
  if (error.isEmpty()) {
    if (!png.isEmpty()) result["png_base64"] = QString::fromLatin1(png.toBase64());
    job.value["result"] = result;
  } else job.value["error"] = error;
  job.bytes = QJsonDocument(job.value).toJson(QJsonDocument::Compact).size();
  if (responseBytes + job.bytes > 128LL * 1024 * 1024) {
    job.value.remove("result");
    job.value["state"] = "failed";
    job.value["error"] = "Response budget exceeded.";
    job.bytes = QJsonDocument(job.value).toJson(QJsonDocument::Compact).size();
  }
  responseBytes += job.bytes;
}
void Transport::expire()
{
  for (auto it = snapshot.readExpiry.begin(); it != snapshot.readExpiry.end();) {
    if (it->hasExpired()) { snapshot.reads.remove(it.key()); it = snapshot.readExpiry.erase(it); }
    else ++it;
  }
  for (auto it = jobs.begin(); it != jobs.end(); ++it) {
    auto state = it->value.value("state").toString();
    if (state == "queued" && now() > it->deadline) {
      it->value["state"] = "cancelled";
      it->value["error"] = "Queue start deadline exceeded; execution did not start.";
      it->created = now();
    }
    if (state != "running" && state != "queued" && state != "expired" && now() - it->created > 300000) {
      responseBytes -= it->bytes;
      it->bytes = 0;
      it->value.remove("result");
      it->value.remove("error");
      it->value["terminal_state"] = it->value.value("state");
      it->value["state"] = "expired";
    }
  }
}
bool Transport::beginJob(const QString& server, const QString& id)
{
  expire();
  if (!activeJob(server, id)) return false;
  jobs[id].value["state"] = "running";
  jobs[id].value["execution_started"] = true;
  return true;
}

void Transport::routeV2(QTcpSocket *socket, const QByteArray& method, const QString& path,
                        const QByteArray& bytes)
{
  expire();
  if (method == "GET" && !bytes.isEmpty()) {
    respond(socket, 400, {{"code", "invalid_request"}, {"message", "GET body is unsupported."}});
    return;
  }
  if (method == "GET" && (path == "/v2/info" || path == "/v2/status")) {
    auto value = path == "/v2/info" ? info : snapshot.status;
    value["api_version"] = 2;
    value["capabilities"] = QJsonArray{"status.read", "requests.read", "requests.cancel", "documents.read",
      "source.read", "attempts.read", "results.read", "diagnostics.read", "syntax.parse", "trees.read", "dependencies.read"};
    value["state_generation"] = QString::number(stateGeneration);
    value["server_id"] = info.value("server_id");
    value["gui_heartbeat_age_ms"] = heartbeat ? double(now() - heartbeat) : -1;
    value["gui_responsive"] = heartbeat && now() - heartbeat < 5000;
    value["limits"] = QJsonObject{{"body_bytes", MaxBody}, {"queue", 32}, {"receipts", 10000},
                                  {"retained_results", 128}, {"result_ttl_seconds", 300}};
    respond(socket, 200, value);
    return;
  }
  if (method == "GET" && (path == "/v2/documents" || path == "/v2/viewports")) {
    const auto key = path.mid(4);
    respond(socket, 200, {{key, snapshot.status.value(key)}, {"state_generation", QString::number(stateGeneration)}});
    return;
  }
  if (method == "GET" && snapshot.reads.contains(path)) {
    respond(socket, 200, snapshot.reads.value(path));
    return;
  }
  if (method == "GET" && (path.startsWith("/v2/source-snapshots/") || path.startsWith("/v2/trees/"))) {
    respond(socket, 410, {{"code", "expired"}, {"message", "Snapshot/page unavailable; capture explicitly again."}});
    return;
  }
  if (method == "POST" && QRegularExpression("^/v2/documents/[a-fA-F0-9-]{36}/(source-snapshots|parse)$").match(path).hasMatch()) {
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(bytes, &error);
    auto body = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        !validId(body.value("request_id").toString()) || body.value("server_id") != info.value("server_id") ||
        body.value("document_id") != path.section('/', 3, 3) || !validId(body.value("window_id").toString()) ||
        (path.endsWith("/parse") ? !validId(body.value("snapshot_id").toString()) :
          (body.value("origin") != "disk" && body.value("origin") != "editor"))) {
      respond(socket, 400, {{"code", "invalid_request"}, {"message", "Explicit document/window, origin, server and request required."}});
      return;
    }
    enqueueV2(socket, path, body);
    return;
  }
  if (method == "GET" && path.startsWith("/v2/requests/")) {
    const auto id = path.mid(13);
    if (!jobs.contains(id)) respond(socket, 410, {{"code", "expired"}, {"message", "Unknown request; do not resubmit under a new ID."}});
    else respond(socket, jobs[id].value.value("state") == "expired" ? 410 : 200, jobs[id].value);
    return;
  }
  if (method == "POST" && QRegularExpression("^/v2/requests/[a-fA-F0-9-]{36}/cancel$").match(path).hasMatch()) {
    QJsonParseError parse;
    auto document = QJsonDocument::fromJson(bytes, &parse);
    const auto body = document.object();
    const auto id = body.value("request_id").toString(), target = path.section('/', 3, 3);
    if (parse.error != QJsonParseError::NoError || !document.isObject() || !validId(id) ||
        body.value("server_id") != info.value("server_id")) {
      respond(socket, 400, {{"code", "invalid_request"}, {"message", "Server and request ID required."}});
      return;
    }
    const auto fingerprint = QCryptographicHash::hash(path.toUtf8() + QJsonDocument(body).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (jobs.contains(id)) {
      respond(socket, jobs[id].input == fingerprint ? 200 : 409, jobs[id].input == fingerprint ? jobs[id].value : QJsonObject{{"code", "request_conflict"}});
      return;
    }
    int retained = 0;
    for (const auto& job : jobs)
      if (job.value.value("state") != "expired") ++retained;
    if (jobs.size() >= 10000 || retained >= 128) {
      respond(socket, 429, {{"code", "capacity"}, {"message", "Receipt capacity reached."}});
      return;
    }
    if (!jobs.contains(target)) {
      respond(socket, 410, {{"code", "expired"}, {"message", "Unknown target request."}});
      return;
    }
    auto& targetJob = jobs[target];
    const bool cancelled = targetJob.value.value("state") == "queued";
    if (cancelled) {
      targetJob.value["state"] = "cancelled";
      targetJob.created = now();
    }
    QJsonObject receipt{{"request_id", id}, {"server_id", info.value("server_id")},
                        {"state", "succeeded"}, {"execution_started", true},
                        {"result", QJsonObject{{"cancelled", cancelled}, {"target_state", targetJob.value.value("state")}}}};
    jobs.insert(id, {fingerprint, receipt, now(), 0, 0});
    respond(socket, 200, receipt);
    return;
  }
  respond(socket, 404, {{"code", "unsupported"}, {"message", "Operation is not available."}});
}
void Transport::enqueueV2(QTcpSocket *socket, const QString& path, const QJsonObject& body)
{
  const auto id = body.value("request_id").toString();
  const auto fingerprint = QCryptographicHash::hash(path.toUtf8() + '\n' + QJsonDocument(body).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
  if (jobs.contains(id)) {
    respond(socket, jobs[id].input == fingerprint ? 200 : 409, jobs[id].input == fingerprint ? jobs[id].value : QJsonObject{{"code", "request_conflict"}});
    return;
  }
  if (!heartbeat || now() - heartbeat >= 5000) {
    respond(socket, 503, {{"code", "gui_unresponsive"}, {"message", "Wait for a fresh GUI heartbeat."}});
    return;
  }
  int pending = 0, retained = 0;
  for (const auto& job : jobs) {
    const auto state = job.value.value("state");
    if (state == "queued" || state == "running") ++pending;
    if (state != "expired") ++retained;
  }
  if (pending >= 32 || retained >= 128 || jobs.size() >= 10000 || responseBytes >= 128LL * 1024 * 1024) {
    respond(socket, 429, {{"code", "capacity"}, {"message", "Request capacity reached."}});
    return;
  }
  QJsonObject receipt{{"request_id", id}, {"server_id", info.value("server_id")},
    {"document_id", body.value("document_id")}, {"state", "queued"}, {"execution_started", false}};
  jobs.insert(id, {fingerprint, receipt, now(), 0, now() + 10000});
  respond(socket, 202, receipt);
  emit operation(info.value("server_id").toString(), id, path, body);
}
}  // namespace AgentAnnotations
