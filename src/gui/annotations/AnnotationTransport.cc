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

// Explicit initialization also retains this resource when linked through a static library.
static void initAgentApiResources()
{
  Q_INIT_RESOURCE(agent_api);
}

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
  static const bool initialized = [] { initAgentApiResources(); return true; }();
  Q_UNUSED(initialized);
}
qint64 Transport::now() const
{
  if (clock) return clock();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
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
          {"api_version", 1},
          {"schema_version", 1},
          {"server_id", id},
          {"url", url},
          {"links", QJsonObject{{"help", "/v1/help"}, {"schema", "/v1/schema"},
            {"contract", "/v1/help/contract"}, {"status", "/v1/status"}}},
          {"pid", QString::number(QCoreApplication::applicationPid())},
          {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
          {"executable", QCoreApplication::applicationFilePath()},
          {"capabilities", QJsonArray{"api.help", "images.read", "status.read", "requests.read", "requests.cancel", "documents.read",
            "source.read", "attempts.read", "results.read", "diagnostics.read", "syntax.parse", "trees.read",
            "dependencies.read", "model.read", "parameters.read", "annotations.read", "camera.set",
            "camera.restore", "viewport.capture", "saved_views.read", "saved_views.write"}}};
  QString directory =
    QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/OpenSCAD/AgentServer";
  if (!QDir().mkpath(directory)) {
    stop();
    emit listening({}, "Cannot create agent discovery directory.");
    return;
  }
  discovery = directory + "/" + id + ".json";
  QSaveFile file(discovery);
  QByteArray data = QJsonDocument(info).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
    stop();
    emit listening({}, "Cannot publish agent discovery.");
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
  if (code >= 400 && !value.contains("code")) {
    value["code"] = code == 403 ? "transport_guard" : code == 410 ? "expired" : "invalid_request";
    value["message"] = value.take("error").toString("Request failed.");
  }
  if (value.contains("request_id")) {
    value["status_route"] = "/v1/requests/" + value.value("request_id").toString();
    if (value.value("state") == "queued" || value.value("state") == "running") value["poll_after_ms"] = 100;
  }
  if (code >= 400) value["help_route"] = "/v1/help";
  QByteArray data = QJsonDocument(value).toJson(QJsonDocument::Compact);
  if (data.size() > 1024 * 1024) {
    code = 413;
    data = "{\"code\":\"response_limit\",\"message\":\"Response exceeds bounded JSON output; use narrower reads.\"}";
  }
  QByteArray response = "HTTP/1.1 " + QByteArray::number(code) + " " + (code < 300 ? "OK" : "Error") +
                        "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: "
                        "close\r\nContent-Length: " +
                        QByteArray::number(data.size()) + "\r\n\r\n";
  socket->write(response);
  socket->write(data);
  socket->disconnectFromHost();
}
void Transport::respondBytes(QTcpSocket *socket, const QByteArray& data, const QByteArray& type)
{
  if (socket->state() == QAbstractSocket::UnconnectedState) return;
  if (data.size() > Store::MaxPng) {
    respond(socket, 413, {{"code", "response_limit"}}); return;
  }
  const auto hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
  socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type +
    "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Content-SHA256: " + hash +
    "\r\nContent-Length: " + QByteArray::number(data.size()) + "\r\nConnection: close\r\n\r\n");
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
  const bool discoveryRequest = request[0] == "GET" && request[1] == "/v1/info";
  bool browser = fields.contains("origin");
  for (auto it = fields.cbegin(); it != fields.cend(); ++it)
    browser = browser || it.key().startsWith("sec-fetch-");
  const auto host = "127.0.0.1:" + QByteArray::number(listener->serverPort());
  if (fields.value("host") != host || browser ||
      fields.contains("transfer-encoding") || fields.value("x-openscad-agent") != "1" ||
      (!discoveryRequest && fields.value("x-openscad-agent-server") != info.value("server_id").toString().toUtf8())) {
    respond(socket, 403,
      {{"code", "transport_guard"}, {"message", "Numeric loopback Host, agent/server headers and no browser Origin are required."}});
    return;
  }
  const QUrl url = QUrl::fromEncoded(request[1]);
  if (!url.path().startsWith("/v1/")) {
    respond(socket, 404, {{"code", "unsupported"}, {"message", "Operation is not available."}});
    return;
  }
  if (request[0] == "POST" && fields.value("content-type").split(';').first().trimmed() != "application/json") {
    respond(socket, 415, {{"code", "invalid_media_type"}, {"message", "JSON required."}});
    return;
  }
  routeApi(socket, request[0], url.path(), bytes, QUrlQuery(url));
}
void Transport::complete(const QString& serverId, const QString& id, QJsonObject result, QByteArray png,
                         QString error)
{
  if (!listener || info.value("server_id") != serverId || !jobs.contains(id)) return;
  auto& job = jobs[id];
  if (job.value.value("state") != "queued" && job.value.value("state") != "running") return;
  if (png.size() > Store::MaxPng || responseBytes + png.size() > 128LL * 1024 * 1024) {
    error = "Response image budget exceeded.";
    png.clear();
  }
  job.value["state"] = error.isEmpty() ? "succeeded" : "failed";
  job.created = now();  // Retain terminal outcomes for five minutes after completion.
  if (error.isEmpty()) {
    if (!png.isEmpty()) {
      result["image_route"] = "/v1/requests/" + id + "/image";
      result["png_sha256"] = QString::fromLatin1(Store::hash(png));
      result["png_bytes"] = double(png.size());
      job.png = std::move(png);
    }
    job.value["result"] = result;
  } else job.value["error"] = error;
  job.bytes = QJsonDocument(job.value).toJson(QJsonDocument::Compact).size() + job.png.size();
  if (responseBytes + job.bytes > 128LL * 1024 * 1024) {
    job.value.remove("result");
    job.png.clear();
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
      it->png.clear();
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

void Transport::routeApi(QTcpSocket *socket, const QByteArray& method, const QString& path,
                        const QByteArray& bytes, const QUrlQuery& query)
{
  expire();
  if (method == "GET" && !bytes.isEmpty()) {
    respond(socket, 400, {{"code", "invalid_request"}, {"message", "GET body is unsupported."}});
    return;
  }
  if (method == "GET" && (path == "/v1/help" || path == "/v1/schema" || path == "/v1/help/contract")) {
    const auto name = path == "/v1/help" ? "help.json" : path == "/v1/schema" ? "schema.json" : "contract.md";
    QFile resource(QString(":/agent-api/") + name);
    if (!resource.open(QIODevice::ReadOnly)) {
      respond(socket, 503, {{"code", "help_unavailable"}}); return;
    }
    if (path == "/v1/help/contract") respondBytes(socket, resource.readAll(), "text/plain; charset=utf-8");
    else {
      auto value = QJsonDocument::fromJson(resource.readAll()).object();
      value["server_id"] = info.value("server_id");
      respond(socket, 200, value);
    }
    return;
  }
  if (method == "GET" && (path == "/v1/info" || path == "/v1/status")) {
    auto value = path == "/v1/info" ? info : snapshot.status;
    value["api_version"] = info.value("api_version");
    value["schema_version"] = info.value("schema_version");
    value["capabilities"] = info.value("capabilities");
    value["state_generation"] = QString::number(stateGeneration);
    value["server_id"] = info.value("server_id");
    value["gui_heartbeat_age_ms"] = heartbeat ? double(now() - heartbeat) : -1;
    value["gui_responsive"] = heartbeat && now() - heartbeat < 5000;
    value["limits"] = QJsonObject{{"body_bytes", MaxBody}, {"queue", 32}, {"receipts", 10000},
                                  {"retained_results", 128}, {"result_ttl_seconds", 300}};
    respond(socket, 200, value);
    return;
  }
  if (method == "GET" && (path == "/v1/documents" || path == "/v1/viewports")) {
    const auto key = path.mid(4);
    respond(socket, 200, {{key, snapshot.status.value(key)}, {"state_generation", QString::number(stateGeneration)}});
    return;
  }
  if (method == "GET" && snapshot.reads.contains(path)) {
    respond(socket, 200, snapshot.reads.value(path));
    return;
  }
  if (method == "GET" && (path == "/v1/annotations" || path.startsWith("/v1/annotations/"))) {
    const auto document = query.queryItemValue("document_id");
    auto found = snapshot.records.constFind(document);
    if (found == snapshot.records.cend()) {
      respond(socket, 404, {{"code", "unknown_target"}, {"message", "Unknown document."}}); return;
    }
    if (path != "/v1/annotations") {
      const auto detailRoute = "/v1/annotations/" + path.section('/', 3, 3);
      if (path != detailRoute && path != detailRoute + "/image") {
        respond(socket, 404, {{"code", "unsupported"}}); return;
      }
      for (const auto& record : *found) if (record.id() == path.section('/', 3, 3)) {
        if (path.endsWith("/image")) { respondBytes(socket, record.png, "image/png"); return; }
        auto result = record.metadata;
        result["document_id"] = document;
        result["image_route"] = detailRoute + "/image?document_id=" + QString::fromLatin1(QUrl::toPercentEncoding(document));
        result["png_sha256"] = QString::fromLatin1(Store::hash(record.png));
        result["png_bytes"] = double(record.png.size());
        respond(socket, 200, result); return;
      }
      respond(socket, 404, {{"code", "unknown_annotation"}}); return;
    }
    bool valid = false;
    const auto offsetText = query.queryItemValue("offset");
    const int offset = offsetText.isEmpty() ? 0 : offsetText.toInt(&valid);
    if ((!offsetText.isEmpty() && !valid) || offset < 0) { respond(socket, 400, {{"code", "invalid_offset"}}); return; }
    QJsonArray values;
    const auto text = query.queryItemValue("q");
    int index = 0, bytes = 0;
    bool more = false;
    for (auto it = found->crbegin(); it != found->crend(); ++it) {
      if (!it->metadata.value("comment").toString().contains(text, Qt::CaseInsensitive)) continue;
      if (index++ < offset) continue;
      const auto size = QJsonDocument(it->metadata).toJson(QJsonDocument::Compact).size();
      if (bytes + size > 768 * 1024 || values.size() >= 100) { --index; more = true; break; }
      values.append(it->metadata); bytes += size;
    }
    QJsonValue next(QJsonValue::Null);
    if (more && !values.isEmpty()) {
      QUrlQuery cursor; cursor.addQueryItem("document_id", document); cursor.addQueryItem("q", text);
      cursor.addQueryItem("offset", QString::number(index));
      next = "/v1/annotations?" + cursor.toString(QUrl::FullyEncoded);
    }
    if (values.isEmpty() && more) {
      respond(socket, 413, {{"code", "metadata_limit"}, {"message", "Read individual evidence by ID."}}); return;
    }
    respond(socket, 200, {{"document_id", document}, {"annotations", values}, {"next_cursor", next}}); return;
  }
  if (method == "POST" && (path == "/v1/capture" ||
      QRegularExpression("^/v1/(viewports/[a-fA-F0-9-]{36}/view|annotations/[a-fA-F0-9-]{36}/restore)$").match(path).hasMatch())) {
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    const auto body = doc.object();
    if (parse.error != QJsonParseError::NoError || !doc.isObject() ||
        body.value("server_id") != info.value("server_id") || !validId(body.value("request_id").toString()) ||
        !validId(body.value("document_id").toString()) || !validId(body.value("window_id").toString()) ||
        !validId(body.value("viewport_id").toString()) || body.value("expected_result_id").toString().isEmpty() ||
        body.value("expected_view_revision").toString().isEmpty() ||
        (path.startsWith("/v1/viewports/") && path.section('/', 3, 3) != body.value("viewport_id"))) {
      respond(socket, 400, {{"code", "invalid_request"}, {"message", "Explicit target and expected result/view required."}}); return;
    }
    enqueue(socket, path, body); return;
  }
  if (method == "GET" && (path.startsWith("/v1/source-snapshots/") || path.startsWith("/v1/trees/") || path.contains("/saved-views/pages/"))) {
    respond(socket, 410, {{"code", "expired"}, {"message", "Snapshot/page unavailable; capture explicitly again."}});
    return;
  }
  if (method == "POST" && QRegularExpression("^/v1/documents/[a-fA-F0-9-]{36}/(source-snapshots|parse)$").match(path).hasMatch()) {
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
    enqueue(socket, path, body);
    return;
  }
  if (method == "POST" && QRegularExpression("^/v1/documents/[a-fA-F0-9-]{36}/saved-views(/[a-fA-F0-9-]{36}/(rename|remove|recall))?$").match(path).hasMatch()) {
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    const auto body = doc.object();
    const bool camera = path.endsWith("/saved-views") || path.endsWith("/recall");
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || !validId(body.value("request_id").toString()) ||
        body.value("server_id") != info.value("server_id") || body.value("document_id") != path.section('/', 3, 3) ||
        !validId(body.value("window_id").toString()) || !validId(body.value("viewport_id").toString()) ||
        body.value("expected_library_revision").toString().isEmpty() ||
        (camera && (body.value("expected_result_id").toString().isEmpty() || body.value("expected_view_revision").toString().isEmpty())) ||
        (path.endsWith("/rename") && !body.value("name").isString())) {
      respond(socket, 400, {{"code", "invalid_request"}, {"message", "Saved views require explicit target/library guards and camera guards for add/recall."}}); return;
    }
    enqueue(socket, path, body); return;
  }
  if (method == "GET" && path.startsWith("/v1/requests/")) {
    const auto id = path.section('/', 3, 3);
    const auto receiptRoute = "/v1/requests/" + id;
    if (path != receiptRoute && path != receiptRoute + "/image") {
      respond(socket, 404, {{"code", "unsupported"}}); return;
    }
    if (!jobs.contains(id)) respond(socket, 410, {{"code", "expired"}, {"message", "Unknown request; do not resubmit under a new ID."}});
    else if (path.endsWith("/image")) {
      const auto& job = jobs[id];
      if (job.value.value("state") == "expired") respond(socket, 410, {{"code", "expired"}});
      else if (job.value.value("state") == "queued" || job.value.value("state") == "running")
        respond(socket, 409, {{"code", "not_ready"}, {"status_route", receiptRoute}, {"poll_after_ms", 100}});
      else if (job.png.isEmpty()) respond(socket, 404, {{"code", "no_image"}});
      else respondBytes(socket, job.png, "image/png");
    }
    else respond(socket, jobs[id].value.value("state") == "expired" ? 410 : 200, jobs[id].value);
    return;
  }
  if (method == "POST" && QRegularExpression("^/v1/requests/[a-fA-F0-9-]{36}/cancel$").match(path).hasMatch()) {
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
void Transport::enqueue(QTcpSocket *socket, const QString& path, const QJsonObject& body)
{
  const auto id = body.value("request_id").toString();
  const auto fingerprint = QCryptographicHash::hash(path.toUtf8() + '\n' + QJsonDocument(body).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
  if (jobs.contains(id)) {
    respond(socket, jobs[id].input == fingerprint ? 200 : 409, jobs[id].input == fingerprint ? jobs[id].value : QJsonObject{{"code", "request_conflict"}});
    return;
  }
  if (path.contains("/saved-views")) {
    const auto state = snapshot.reads.value("/v1/documents/" + body.value("document_id").toString() + "/saved-views");
    if (!state.value("available").toBool() || state.value("window_id") != body.value("window_id") ||
        state.value("viewport_id") != body.value("viewport_id") || state.value("library_revision") != body.value("expected_library_revision")) {
      respond(socket, 409, {{"code", "stale_library"}, {"message", "Saved-view library or displayed owner changed."}}); return;
    }
  }
  if (body.contains("expected_view_revision") || path.contains("/saved-views")) {
    bool matched = false;
    for (const auto& entry : snapshot.status.value("viewports").toArray()) {
      const auto view = entry.toObject();
      if (view.value("viewport_id") == body.value("viewport_id") && view.value("window_id") == body.value("window_id") &&
          view.value("document_id") == body.value("document_id") && !view.value("busy").toBool() &&
          (!body.contains("expected_view_revision") || (view.value("displayed_result_id") == body.value("expected_result_id") &&
          view.value("view_revision") == body.value("expected_view_revision")))) matched = true;
    }
    if (!matched) { respond(socket, 409, {{"code", "stale_or_busy"}, {"message", "Refresh target/result/view after resolving busy interaction."}}); return; }
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
