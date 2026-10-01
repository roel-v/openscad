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
  QPair<int, QJsonObject> request(const QJsonObject& info, QString path, QByteArray body = {},
                                  QByteArray extraName = {}, QByteArray extraValue = {},
                                  bool post = false)
  {
    QNetworkRequest request(QUrl(info.value("url").toString() + path));
    request.setRawHeader("X-Agent-Annotations", "1");
    request.setRawHeader("X-Agent-Annotations-Server", info.value("server_id").toString().toUtf8());
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
    auto result = qMakePair(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                            QJsonDocument::fromJson(reply->readAll()).object());
    reply->abort();
    reply->deleteLater();
    return result;
  }
private slots:
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
                                                            {"result", QJsonObject{{"id", "1"}}}}}}};
    transport.publish(snapshot);
    QCOMPARE(request(info, "/v1/info").first, 200);
    QCOMPARE(request(info, "/v1/info", {}, "Origin", "http://example.invalid").first, 403);
    QCOMPARE(request(info, "/v1/info", {}, "Host", "example.invalid").first, 403);
    QCOMPARE(request(info, "/v1/info", {}, "X-Agent-Annotations-Server", uuid().toUtf8()).first, 403);
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
                     {"expected_result_id", "1"}};
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
                                                            {"result", QJsonObject{{"id", "1"}}}}}}};
    snapshot.records.insert(document, {});
    transport.publish(snapshot);
    QJsonObject body{{"server_id", info.value("server_id")},
                     {"request_id", uuid()},
                     {"document_id", document},
                     {"window_id", window},
                     {"viewport_id", viewport},
                     {"expected_result_id", "1"}};
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
