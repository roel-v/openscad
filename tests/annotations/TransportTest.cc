#include "../../src/gui/annotations/AnnotationTransport.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>
using namespace AgentAnnotations;
static QString uuid()
{
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
class TransportTest : public QObject
{
  Q_OBJECT
  QNetworkAccessManager network;
  QByteArray lastBytes, lastType, lastHash;
  QPair<int, QJsonObject> request(const QJsonObject& info, QString path, QByteArray body = {},
                                  QByteArray extraName = {}, QByteArray extraValue = {},
                                  bool post = false)
  {
    QNetworkRequest request(QUrl(info.value("url").toString() + path));
    request.setRawHeader("X-OpenSCAD-Agent", "1");
    request.setRawHeader("X-OpenSCAD-Agent-Server", info.value("server_id").toString().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!extraName.isEmpty()) request.setRawHeader(extraName, extraValue);
    auto reply = post ? network.post(request, body) : network.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(5000);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    loop.exec();
    lastBytes = reply->readAll();
    lastType = reply->rawHeader("Content-Type");
    lastHash = reply->rawHeader("X-Content-SHA256");
    auto result = qMakePair(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                            QJsonDocument::fromJson(lastBytes).object());
    reply->abort();
    reply->deleteLater();
    return result;
  }
private slots:
  void cameraUpdatesPreserveEvidence()
  {
    Transport transport;
    transport.start();
    const auto info = transport.connection();
    const auto viewport = uuid();
    const QJsonObject evidence{{"result_id", "retained:1"}};
    Snapshot snapshot;
    snapshot.status = {{"viewports", QJsonArray{QJsonObject{{"viewport_id", viewport},
      {"view_revision", "1"}, {"evaluation", evidence}, {"busy", false}}}}};
    snapshot.reads.insert("/v1/test-evidence", evidence);
    transport.publish(snapshot);
    const auto before = request(info, "/v1/status").second;
    const QJsonObject camera{{"distance", 42}};
    transport.publishCameras({QJsonObject{{"viewport_id", viewport}, {"camera", camera}, {"view_revision", "2"}}});
    const auto after = request(info, "/v1/status").second;
    const auto view = after.value("viewports").toArray().first().toObject();
    QCOMPARE(view.value("camera").toObject(), camera);
    QCOMPARE(view.value("view_revision").toString(), QString("2"));
    QCOMPARE(view.value("evaluation").toObject(), evidence);
    QCOMPARE(view.value("busy").toBool(), false);
    QVERIFY(before.value("state_generation") != after.value("state_generation"));
    QCOMPARE(request(info, "/v1/test-evidence").second, evidence);
    transport.publishCameras({QJsonObject{{"viewport_id", uuid()}, {"view_revision", "99"}}});
    QCOMPARE(request(info, "/v1/status").second.value("viewports"), after.value("viewports"));
  }
  void directHelpAndImages()
  {
    qint64 time = 1000;
    Transport transport(nullptr, [&] { return time; });
    transport.start();
    const auto info = transport.connection();
    const auto help = request(info, info.value("links").toObject().value("help").toString());
    QCOMPARE(help.first, 200);
    QVERIFY(help.second.value("routes").toArray().size() >= 30);
    QVERIFY(request(info, "/v1/schema").second.contains("$defs"));
    QCOMPARE(request(info, "/v1/help/contract").first, 200);
    QVERIFY(lastType.startsWith("text/plain"));
    QVERIFY(lastBytes.contains("Self-description"));
    QCOMPARE(request(info, "/v1/help", {}, "X-OpenSCAD-Agent-Server", uuid().toUtf8()).first, 403);
    QCOMPARE(request(info, "/v1/help", {}, "Origin", "https://example.com").first, 403);
    const auto document = uuid(), window = uuid(), viewport = uuid(), id = uuid();
    Snapshot snapshot;
    snapshot.status = {{"viewports", QJsonArray{QJsonObject{{"document_id", document},
      {"window_id", window}, {"viewport_id", viewport}, {"busy", false},
      {"displayed_result_id", "qualified:1"}, {"view_revision", "4"}}}}};
    transport.publish(snapshot);
    QJsonObject body{{"server_id", info.value("server_id")}, {"request_id", id},
      {"document_id", document}, {"window_id", window}, {"viewport_id", viewport},
      {"expected_result_id", "qualified:1"}, {"expected_view_revision", "4"}};
    const auto submitted = request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true);
    QCOMPARE(submitted.first, 202);
    const auto route = submitted.second.value("status_route").toString();
    QCOMPARE(route, "/v1/requests/" + id);
    QCOMPARE(submitted.second.value("poll_after_ms").toInt(), 100);
    QCOMPARE(request(info, route + "/image").first, 409);
    const QByteArray png("detached PNG bytes"); // Transport preserves exact bytes; native tests validate actual PNGs.
    QVERIFY(transport.beginJob(info.value("server_id").toString(), id));
    transport.complete(info.value("server_id").toString(), id, {}, png, {});
    const auto result = request(info, route).second.value("result").toObject();
    QVERIFY(!result.contains("png_base64"));
    QCOMPARE(result.value("png_bytes").toInt(), png.size());
    const auto imageRoute = result.value("image_route").toString();
    QCOMPARE(request(info, imageRoute).first, 200);
    QCOMPARE(lastBytes, png);
    QCOMPARE(lastType, "image/png");
    QCOMPARE(lastHash, Store::hash(png));
    QCOMPARE(result.value("png_sha256").toString(), QString::fromLatin1(lastHash));
    QCOMPARE(request(info, imageRoute, {}, "X-OpenSCAD-Agent-Server", uuid().toUtf8()).first, 403);
    QCOMPARE(request(info, imageRoute, {}, "Origin", "https://example.com").first, 403);
    time += 300001;
    QCOMPARE(request(info, imageRoute).first, 410);
    QCOMPARE(request(info, route).second.value("state").toString(), "expired");
    transport.publish(snapshot);
    body["request_id"] = uuid();
    const auto nextId = body.value("request_id").toString();
    QCOMPARE(request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true).first, 202);
    transport.complete(info.value("server_id").toString(), nextId, {}, {}, "capture failed");
    QCOMPARE(request(info, "/v1/requests/" + nextId + "/image").first, 404);
    transport.stop(); transport.start();
    QCOMPARE(request(transport.connection(), imageRoute).first, 410);
  }
  void evidenceAndAdmission()
  {
    Transport transport;
    transport.start();
    auto info = transport.connection();
    const auto document = uuid(), viewport = uuid(), window = uuid(), annotation = uuid();
    Snapshot snapshot;
    const QJsonObject view{{"document_id", document}, {"window_id", window}, {"viewport_id", viewport},
      {"displayed_result_id", "qualified:1"}, {"view_revision", "4"}, {"busy", false}};
    snapshot.status = {{"viewports", QJsonArray{view}}};
    Record record{{{"id", annotation}, {"comment", "Inspect HOLE"}}, QByteArray("immutable evidence")};
    snapshot.records.insert(document, {record});
    transport.publish(snapshot);
    auto found = request(info, "/v1/annotations?document_id=" + document + "&q=hole");
    QCOMPARE(found.second.value("annotations").toArray().size(), 1);
    QVERIFY(found.second.value("next_cursor").isNull());
    auto detail = request(info, "/v1/annotations/" + annotation + "?document_id=" + document);
    QVERIFY(!detail.second.contains("png_base64"));
    const auto imageRoute = detail.second.value("image_route").toString();
    QCOMPARE(request(info, imageRoute).first, 200);
    QCOMPARE(lastBytes, record.png);
    QCOMPARE(lastHash, Store::hash(record.png));
    QCOMPARE(request(info, imageRoute + "/extra").first, 404);
    QJsonObject body{{"server_id", info.value("server_id")}, {"request_id", uuid()}, {"document_id", document},
      {"window_id", window}, {"viewport_id", viewport}, {"expected_result_id", "qualified:1"}, {"expected_view_revision", "4"}};
    QSignalSpy dispatched(&transport, &Transport::operation);
    auto submit = [&] { return request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true); };
    QCOMPARE(submit().first, 202);
    auto busy = view; busy["busy"] = true; busy["view_revision"] = "5";
    snapshot.status["viewports"] = QJsonArray{busy}; transport.publish(snapshot);
    QCOMPARE(submit().first, 200); // Identical retry is still the original receipt.
    QCOMPARE(dispatched.size(), 1);
    body["request_id"] = uuid();
    QCOMPARE(submit().first, 409);
    QCOMPARE(request(info, "/v1/annotations/" + annotation + "?document_id=" + document).second, detail.second);
    body.remove("expected_view_revision");
    QCOMPARE(submit().first, 400);
    snapshot.records.clear(); transport.publish(snapshot);
    QCOMPARE(request(info, imageRoute).first, 404);
  }
  void queueCapacity()
  {
    Transport transport;
    transport.start();
    auto info = transport.connection();
    transport.publish({});
    const auto document = uuid(), window = uuid();
    QJsonObject body{{"server_id", info.value("server_id")}, {"request_id", uuid()},
      {"document_id", document}, {"window_id", window}, {"origin", "disk"}};
    for (int i = 0; i < 32; ++i) {
      body["request_id"] = uuid();
      QCOMPARE(request(info, "/v1/documents/" + document + "/source-snapshots", QJsonDocument(body).toJson(), {}, {}, true).first, 202);
    }
    body["request_id"] = uuid();
    QCOMPARE(request(info, "/v1/documents/" + document + "/source-snapshots", QJsonDocument(body).toJson(), {}, {}, true).first, 429);
  }
  void cachedInspectionExpiry()
  {
    Transport transport;
    transport.start();
    Snapshot snapshot;
    const auto route = "/v1/trees/" + uuid();
    snapshot.reads.insert(route, {{"kind", "syntax"}});
    snapshot.readExpiry.insert(route, QDeadlineTimer(0));
    transport.publish(snapshot);
    QCOMPARE(request(transport.connection(), route).first, 410);
    QCOMPARE(request(transport.connection(), "/v1/info").second.value("gui_responsive").toBool(), true);
  }
  void versionAndGuards()
  {
    Transport transport;
    transport.start();
    const auto info = transport.connection();
    const auto live = request(info, "/v1/info");
    QCOMPARE(live.first, 200);
    QCOMPARE(live.second.value("api_version").toInt(), 1);
    QCOMPARE(live.second.value("schema_version").toInt(), 1);
    QVERIFY(!live.second.contains("protocol"));
    QVERIFY(!live.second.contains("schema"));
    QVERIFY(!live.second.contains("agent_api_versions"));
    QVERIFY(!live.second.contains("access_mode"));
    QVERIFY(live.second.value("capabilities").toArray().contains("source.read"));
    QCOMPARE(request(info, "/v2/info").first, 404);
    QCOMPARE(request(info, "/v2/capture", "{}", {}, {}, true).first, 404);
    QCOMPARE(request(info, "/v1/info", {}, "X-OpenSCAD-Agent", "").first, 403);
    QCOMPARE(request(info, "/v1/info", {}, "X-OpenSCAD-Agent-Server", "").first, 200);
    QCOMPARE(request(info, "/v1/status", {}, "X-OpenSCAD-Agent-Server", uuid().toUtf8()).first, 403);
    QCOMPARE(request(info, "/v1/info", {}, "Sec-Fetch-Site", "none").first, 403);
    QCOMPARE(request(info, "/v1/info", {}, "Sec-Fetch-Mode", "cors").first, 403);
    QCOMPARE(request(info, "/v1/info", {}, "Origin", "null").first, 403);
    QCOMPARE(request(info, "/v1/execute", "{}", {}, {}, true).first, 404);
    QNetworkRequest oldHeaders(QUrl(info.value("url").toString() + "/v1/info"));
    oldHeaders.setRawHeader("X-Agent-Annotations", "1");
    oldHeaders.setRawHeader("X-Agent-Annotations-Server", info.value("server_id").toString().toUtf8());
    auto reply = network.get(oldHeaders);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    QVERIFY(finished.wait(5000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
    reply->deleteLater();
    Transport second;
    second.start();
    QVERIFY(second.connection().value("server_id") != info.value("server_id"));
    QVERIFY(second.connection().value("url") != info.value("url"));
  }
  void receiptsCancellationAndQueueDeadline()
  {
    qint64 time = 1000;
    Transport transport(nullptr, [&] { return time; });
    transport.start();
    const auto info = transport.connection();
    const auto document = uuid(), window = uuid(), viewport = uuid();
    Snapshot snapshot;
    snapshot.status = {{"viewports", QJsonArray{QJsonObject{{"document_id", document},
      {"window_id", window}, {"viewport_id", viewport}, {"busy", false},
      {"displayed_result_id", "qualified:1"}, {"view_revision", "4"}}}}};
    transport.publish(snapshot);
    QJsonObject body{{"server_id", info.value("server_id")}, {"request_id", uuid()},
      {"document_id", document}, {"window_id", window}, {"viewport_id", viewport}, {"expected_result_id", "qualified:1"}, {"expected_view_revision", "4"}};
    QSignalSpy dispatched(&transport, &Transport::operation);
    auto submit = [&] { return request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true); };
    QCOMPARE(submit().first, 202);
    auto id = body.value("request_id").toString();
    QVERIFY(transport.beginJob(info.value("server_id").toString(), id));
    QVERIFY(!transport.beginJob(info.value("server_id").toString(), id));
    QCOMPARE(request(info, "/v1/requests/" + id).second.value("state").toString(), QString("running"));
    QJsonObject runningCancel{{"server_id", info.value("server_id")}, {"request_id", uuid()}};
    auto runningReply = request(info, "/v1/requests/" + id + "/cancel", QJsonDocument(runningCancel).toJson(), {}, {}, true);
    QCOMPARE(runningReply.second.value("result").toObject().value("cancelled").toBool(), false);
    transport.complete(info.value("server_id").toString(), id, {{"ok", true}}, {}, {});
    time += 300001;
    QCOMPARE(request(info, "/v1/requests/" + id).first, 410);
    QCOMPARE(submit().second.value("state").toString(), QString("expired"));
    QCOMPARE(dispatched.size(), 1);
    transport.publish(snapshot);
    body["request_id"] = uuid();
    QCOMPARE(submit().first, 202);
    id = body.value("request_id").toString();
    QJsonObject cancel{{"server_id", info.value("server_id")}, {"request_id", uuid()}};
    auto cancelBytes = QJsonDocument(cancel).toJson();
    auto route = "/v1/requests/" + id + "/cancel";
    QCOMPARE(request(info, route, cancelBytes, {}, {}, true).second.value("result").toObject().value("cancelled").toBool(), true);
    QVERIFY(!transport.beginJob(info.value("server_id").toString(), id));
    QCOMPARE(request(info, route, cancelBytes, {}, {}, true).first, 200);
    body["request_id"] = uuid();
    QCOMPARE(submit().first, 202);
    id = body.value("request_id").toString();
    time += 10001;
    QVERIFY(!transport.beginJob(info.value("server_id").toString(), id));
    auto receipt = request(info, "/v1/requests/" + id).second;
    QCOMPARE(receipt.value("state").toString(), QString("cancelled"));
    QCOMPARE(receipt.value("execution_started").toBool(), false);
    transport.stop();
    transport.start();
    transport.complete(info.value("server_id").toString(), id, {{"wrong", true}}, {}, {});
    QVERIFY(!transport.beginJob(info.value("server_id").toString(), id));
  }
  void protocolAndIdempotency()
  {
    QStandardPaths::setTestModeEnabled(true);
    Transport transport;
    transport.start();
    const auto info = transport.connection();
    QVERIFY(!info.isEmpty());
    const auto document = uuid(), window = uuid(), viewport = uuid(), id = uuid();
    Snapshot snapshot;
    snapshot.records.insert(document, {});
    snapshot.status = {{"documents", QJsonArray{QJsonObject{{"document_id", document}}}},
                       {"viewports", QJsonArray{QJsonObject{{"document_id", document},
                                                            {"window_id", window},
                                                            {"viewport_id", viewport},
                                                            {"busy", false},
                                                            {"displayed_result_id", "qualified:1"}, {"view_revision", "4"}}}}};
    transport.publish(snapshot);
    QCOMPARE(request(info, "/v1/info").first, 200);
    QCOMPARE(request(info, "/v1/info", {}, "Origin", "http://example.invalid").first, 403);
    QCOMPARE(request(info, "/v1/info", {}, "Host", "example.invalid").first, 403);
    QCOMPARE(request(info, "/v1/status", {}, "X-OpenSCAD-Agent-Server", uuid().toUtf8()).first, 403);
    QCOMPARE(request(info, "/v1/annotations?document_id=" + document)
               .second.value("annotations")
               .toArray()
               .size(),
             0);
    QCOMPARE(request(info, "/v1/annotations?document_id=" + uuid()).first, 404);
    QJsonObject body{{"server_id", info.value("server_id")},
                     {"request_id", id},
                     {"document_id", document},
                     {"window_id", window},
                     {"viewport_id", viewport},
                     {"expected_result_id", "qualified:1"}, {"expected_view_revision", "4"}};
    QSignalSpy jobs(&transport, &Transport::operation);
    auto bytes = QJsonDocument(body).toJson();
    QCOMPARE(request(info, "/v1/capture", bytes, {}, {}, true).first, 202);
    QCOMPARE(jobs.size(), 1);
    QCOMPARE(request(info, "/v1/capture", bytes, {}, {}, true).first, 200);
    QCOMPARE(jobs.size(), 1);
    body["expected_result_id"] = "2";
    QCOMPARE(request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true).first, 409);
    transport.complete(info.value("server_id").toString(), id, {{"verified", true}}, {}, {});
    QCOMPARE(request(info, "/v1/requests/" + id).second.value("state").toString(), QString("succeeded"));
    QCOMPARE(request(info, "/v1/capture", bytes, {}, {}, true).second.value("state").toString(),
             QString("succeeded"));
    QCOMPARE(request(info, "/v1/execute", bytes, {}, {}, true).first, 404);
    QCOMPARE(request(info, "/v1/source", bytes, {}, {}, true).first, 404);
    QCOMPARE(request(info, "/v1/capture", "not json", {}, {}, true).first, 400);
    const auto oversized =
      request(info, "/v1/capture", QByteArray(256 * 1024 + 1, 'x'), {}, {}, true).first;
    // Qt may report an early close while still transmitting the rejected oversized body.
    QVERIFY(oversized == 0 || oversized == 400 || oversized == 413);
    QCOMPARE(jobs.size(), 1);
    transport.stop();
    transport.start();
    QVERIFY(transport.connection().value("server_id") != info.value("server_id"));
    transport.stop();
  }
  void busyAndTargetValidation()
  {
    Transport transport;
    transport.start();
    const auto info = transport.connection();
    const auto document = uuid(), window = uuid(), viewport = uuid();
    Snapshot snapshot;
    snapshot.status = {{"viewports", QJsonArray{QJsonObject{{"document_id", document},
                                                            {"window_id", window},
                                                            {"viewport_id", viewport},
                                                            {"busy", true},
                                                            {"displayed_result_id", "qualified:1"}, {"view_revision", "4"}}}}};
    snapshot.records.insert(document, {});
    transport.publish(snapshot);
    QJsonObject body{{"server_id", info.value("server_id")},
                     {"request_id", uuid()},
                     {"document_id", document},
                     {"window_id", window},
                     {"viewport_id", viewport},
                     {"expected_result_id", "qualified:1"}, {"expected_view_revision", "4"}};
    QCOMPARE(request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true).first, 409);
    QCOMPARE(request(info, "/v1/annotations?document_id=" + document).first, 200);
    body["document_id"] = uuid();
    QCOMPARE(request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true).first, 409);
    body.remove("expected_result_id");
    QCOMPARE(request(info, "/v1/capture", QJsonDocument(body).toJson(), {}, {}, true).first, 400);
    transport.stop();
  }
};
QTEST_MAIN(TransportTest)
#include "TransportTest.moc"
