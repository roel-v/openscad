#include "gui/NavigationCube.h"
#include "TestAnnotations.h"
#include "gui/ScintillaEditor.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/annotations/AnnotationBridge.h"
#include "gui/annotations/AgentSyntax.h"
#include "gui/annotations/AgentModel.h"
#include "gui/CameraNavigation.h"
#include "gui/SavedViews.h"
#include "core/parsersettings.h"
#include "core/ParserInspection.h"
#include "core/SourceFile.h"
#include "core/primitives.h"
#include "utils/printutils.h"
#include "gui/Dock.h"
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QListWidget>
#include <QLineEdit>
#include <QFileDialog>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QSettings>

namespace {
QByteArray agentImage(const QJsonObject& info, const QString& route)
{
  QNetworkAccessManager network;
  QNetworkRequest request(QUrl(info.value("url").toString() + route));
  request.setRawHeader("X-OpenSCAD-Agent", "1");
  request.setRawHeader("X-OpenSCAD-Agent-Server", info.value("server_id").toString().toUtf8());
  auto reply = network.get(request);
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  timeout.start(6000);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  loop.exec();
  if (!reply->isFinished() || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
      reply->header(QNetworkRequest::ContentTypeHeader).toString() != "image/png") return {};
  const auto data = reply->readAll();
  if (AgentAnnotations::Store::hash(data) != reply->rawHeader("X-Content-SHA256")) return {};
  return data;
}
QJsonObject agentRequest(const QJsonObject& info, const QString& route, const QJsonObject& body = {})
{
  QNetworkAccessManager network;
  QNetworkRequest request(QUrl(info.value("url").toString() + route));
  request.setRawHeader("X-OpenSCAD-Agent", "1");
  request.setRawHeader("X-OpenSCAD-Agent-Server", info.value("server_id").toString().toUtf8());
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  auto reply = body.isEmpty() ? network.get(request) : network.post(request, QJsonDocument(body).toJson());
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  timeout.start(6000);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  loop.exec();
  auto result = QJsonDocument::fromJson(reply->readAll()).object();
  result["http_status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  return result;
}
}

void TestAnnotations::nativeSavedViews()
{
  window->designActionAutoReload->setChecked(false);
  QTemporaryDir dir;
  QFile source(dir.filePath("views.scad"));
  QVERIFY(source.open(QIODevice::WriteOnly)); source.write("cube([10,5,3]);"); source.close();
  window->tabManager->open(source.fileName());
  auto editor = window->activeEditor;
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  window->qglview->viewAll();
  QTRY_VERIFY(!window->qglview->navigation()->animating());
  auto controller = window->annotationController;
  auto store = SavedViews::forEditor(editor);
  auto bridge = AgentAnnotations::Bridge::instance();
  bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  const auto info = bridge->connection();
  const auto route = "/v1/documents/" + editor->annotations->sessionId + "/saved-views";
  const auto nativeId = store->add(window->qglview->navigation()->current());
  QVERIFY(!nativeId.isEmpty());
  QTRY_COMPARE(agentRequest(info, route).value("views").toArray().size(), 1);
  auto body = [&] {
    const auto state = controller->status();
    return QJsonObject{{"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
      {"server_id", info.value("server_id")}, {"document_id", editor->annotations->sessionId},
      {"window_id", controller->windowId}, {"viewport_id", controller->viewportId},
      {"expected_result_id", state.value("displayed_result_id")}, {"expected_view_revision", state.value("view_revision")},
      {"expected_library_revision", controller->savedViews().value("library_revision")}};
  };
  auto added = body();
  QCOMPARE(agentRequest(info, route, added).value("http_status").toInt(), 202);
  QJsonObject job;
  QTRY_COMPARE((job = agentRequest(info, "/v1/requests/" + added.value("request_id").toString())).value("state").toString(), QString("succeeded"));
  const auto id = job.value("result").toObject().value("added_id").toString();
  QVERIFY(store->find(id));
  QCOMPARE(store->entries().size(), 2);
  QCOMPARE(agentRequest(info, route, added).value("http_status").toInt(), 200);
  QCOMPARE(store->entries().size(), 2);
  QVERIFY(store->dirty()); QVERIFY(!editor->isContentModified());
  QVERIFY(!QFileInfo::exists(SavedViews::companion(source.fileName())));
  const auto original = controller->camera();
  auto rename = body(); rename["name"] = "  Agent view  ";
  rename.remove("expected_result_id"); rename.remove("expected_view_revision");
  QCOMPARE(agentRequest(info, route + "/" + id + "/rename", rename).value("http_status").toInt(), 202);
  QTRY_COMPARE(store->find(id)->name, QString("Agent view"));
  QCOMPARE(controller->camera(), original);
  auto stale = body(); store->rename(id, "Native edit");
  QTRY_COMPARE(agentRequest(info, route + "/" + id + "/remove", stale).value("http_status").toInt(), 409);
  auto changed = original; changed["projection"] = "orthographic"; changed["distance"] = original.value("distance").toDouble() * 1.7;
  QString error; QVERIFY(controller->setCamera(changed, &error));
  auto recall = body();
  QCOMPARE(agentRequest(info, route + "/" + id + "/recall", recall).value("http_status").toInt(), 202);
  QTRY_COMPARE(controller->camera(), original);
  const auto sourceTime = QFileInfo(source.fileName()).lastModified();
  QVERIFY(window->tabManager->save(editor));
  QCOMPARE(QFileInfo(source.fileName()).lastModified(), sourceTime);
  QVERIFY(!store->dirty());
  SavedViews reopened;
  QVERIFY(reopened.load(source.fileName()));
  QCOMPARE(reopened.entries().size(), 2);
  QCOMPARE(reopened.find(id)->name, QString("Native edit"));
  const auto revision = controller->savedViews().value("library_revision");
  window->tabManager->refreshDocument();
  QCOMPARE(controller->savedViews().value("library_revision"), revision);
  auto remove = body();
  QCOMPARE(agentRequest(info, route + "/" + id + "/remove", remove).value("http_status").toInt(), 202);
  QTRY_VERIFY(!store->find(id));
  QCOMPARE(agentRequest(info, route + "/" + id + "/remove", remove).value("http_status").toInt(), 200);
  QCOMPARE(store->entries().size(), 1);
  // A transition snapshot records the currently displayed pose and cancels its destination.
  auto navigation = window->qglview->navigation();
  auto destination = navigation->current();
  destination.rotation += Eigen::Vector3d(30, 20, 10);
  navigation->settings.animate = true;
  navigation->settings.duration = 1000;
  navigation->navigate(destination);
  QTest::qWait(40);
  QVERIFY(navigation->animating());
  const auto displayed = navigation->current();
  error.clear();
  const auto frozen = controller->savedViewOperation("add", {}, body(), &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  const auto frozenId = frozen.value("added_id").toString();
  QVERIFY(store->find(frozenId));
  QVERIFY(store->find(frozenId)->camera == displayed);
  QVERIFY(!navigation->animating());
  QTest::qWait(60);
  QVERIFY(navigation->current() == displayed);
  store->remove(frozenId);
  // Orthographic poses also survive recall after the viewport aspect changes, without fitting.
  QVERIFY(controller->setCamera(changed, &error));
  const auto ortho = controller->savedViewOperation("add", {}, body(), &error);
  const auto orthoId = ortho.value("added_id").toString();
  QVERIFY(store->find(orthoId));
  const auto windowSize = window->size();
  window->resize(windowSize.width() + 120, windowSize.height());
  QTest::qWait(40);
  QVERIFY(controller->setCamera(original, &error));
  auto orthoRecall = body();
  QCOMPARE(agentRequest(info, route + "/" + orthoId + "/recall", orthoRecall).value("http_status").toInt(), 202);
  QTRY_COMPARE(controller->camera(), changed);
  store->remove(orthoId);
  window->resize(windowSize);
  QVERIFY(controller->setCamera(original, &error));
  // Guards bind the displayed owner even after another editor becomes active.
  window->tabManager->actionNew();
  QVERIFY(window->activeEditor != editor);
  QCOMPARE(controller->owner(), editor);
  QCOMPARE(agentRequest(info, route).value("views").toArray().size(), 1);
  controller->startDrawing();
  QCOMPARE(agentRequest(info, route, body()).value("http_status").toInt(), 409);
  controller->cancelDrawing();
  bridge->stop();
}

void TestAnnotations::directHttpWorkflow()
{
  const auto script = qEnvironmentVariable("OPENSCAD_AGENT_TEST_WORKFLOW");
  if (script.isEmpty()) QSKIP("Set OPENSCAD_AGENT_TEST_WORKFLOW to tests/annotations/LiveHttpWorkflow.py.");
  QTemporaryDir dir;
  QFile source(dir.filePath("http.scad"));
  QVERIFY(source.open(QIODevice::WriteOnly)); source.write("cube([10,5,3]);"); source.close();
  window->designActionAutoReload->setChecked(false);
  window->tabManager->open(source.fileName());
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  window->qglview->viewAll();
  auto bridge = AgentAnnotations::Bridge::instance();
  bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  QFile connection(dir.filePath("connection.json"));
  QVERIFY(connection.open(QIODevice::WriteOnly));
  connection.write(QJsonDocument(bridge->connection()).toJson()); connection.close();
  QTimer pump;
  connect(&pump, &QTimer::timeout, window, [this] { window->qglview->grabFramebuffer(); }); pump.start(30);
  QProcess client;
  const auto evidence = QFileInfo(qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_LOG")).dir();
  client.setWorkingDirectory(evidence.absolutePath());
  client.start(qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_PYTHON"),
    {script, connection.fileName(), window->activeEditor->annotations->sessionId,
      window->annotationController->windowId, window->annotationController->viewportId, evidence.absolutePath()});
  QVERIFY(client.waitForStarted());
  QTRY_COMPARE_WITH_TIMEOUT(client.state(), QProcess::NotRunning, 30000);
  auto errors = client.readAllStandardError();
  QVERIFY2(client.exitCode() == 0, errors.constData());
  QFile log(evidence.filePath("http-live.txt"));
  QVERIFY(log.open(QIODevice::WriteOnly)); log.write(client.readAllStandardOutput()); log.write(errors);
  // A real F6 result must retain the same identity/capture guarantees as preview.
  const auto preview = window->annotationController->status().value("displayed_result_id");
  window->designActionRender->trigger();
  QTRY_VERIFY_WITH_TIMEOUT(!window->annotationController->busy(), 15000);
  const auto rendered = window->annotationController->status();
  QVERIFY(rendered.value("displayed_result_id").isString());
  QVERIFY(rendered.value("displayed_result_id") != preview);
  bool captured = false;
  QString captureError;
  window->annotationController->capture(rendered.value("result").toObject().value("result_id").toString(),
    [&](QJsonObject metadata, QByteArray png, QString error) {
      captureError = error;
      captured = true;
      QFile file(evidence.filePath("agent-f6-capture.png"));
      if (file.open(QIODevice::WriteOnly)) file.write(png);
      QCOMPARE(metadata.value("displayed_result_id"), rendered.value("displayed_result_id"));
      const auto image = QImage::fromData(png);
      QVERIFY(!image.isNull());
      int varied = 0;
      for (int y = 0; y < image.height(); y += 4)
        for (int x = 0; x < image.width(); x += 4)
          if (image.pixel(x, y) != image.pixel(0, 0)) ++varied;
      QVERIFY(varied > 500); // A plausible receipt with a blank framebuffer is not evidence.
    });
  QTRY_VERIFY_WITH_TIMEOUT(captured, 6000);
  QVERIFY2(captureError.isEmpty(), qPrintable(captureError));
  bridge->stop();
}

void TestAnnotations::guardedViewport()
{
  window->designActionAutoReload->setChecked(false);
  window->tabManager->actionNew();
  auto editor = window->activeEditor;
  editor->setPlainText("difference(){cube(10,center=true); cylinder(h=20,r=3,center=true);}");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  window->qglview->viewAll();
  QTRY_VERIFY(!window->qglview->navigation()->animating());
  auto controller = window->annotationController;
  auto bridge = AgentAnnotations::Bridge::instance();
  bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  const auto info = bridge->connection();
  auto body = [&] {
    const auto state = controller->status();
    return QJsonObject{{"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
      {"server_id", info.value("server_id")}, {"document_id", editor->annotations->sessionId},
      {"window_id", controller->windowId}, {"viewport_id", controller->viewportId},
      {"expected_result_id", state.value("displayed_result_id")}, {"expected_view_revision", state.value("view_revision")}};
  };
  const auto route = "/v1/viewports/" + controller->viewportId + "/view";
  for (const auto& name : {"top", "bottom", "left", "right", "front", "back", "diagonal"}) {
    auto request = body(); request["operation"] = "standard"; request["view"] = name;
    QCOMPARE(agentRequest(info, route, request).value("http_status").toInt(), 202);
    QTRY_COMPARE(agentRequest(info, "/v1/requests/" + request.value("request_id").toString()).value("state").toString(), QString("succeeded"));
    const auto camera = controller->camera();
    const auto revision = controller->viewRevision();
    // An identical receipt must not move the camera a second time.
    QCOMPARE(agentRequest(info, route, request).value("http_status").toInt(), 200);
    QCOMPARE(controller->viewRevision(), revision);
    QCOMPARE(controller->camera(), camera);
  }
  auto stale = body(); stale["operation"] = "fit";
  window->qglview->cam.setVpr(15,25,35);
  emit window->qglview->cameraChanged();
  QTRY_COMPARE(agentRequest(info, route, stale).value("http_status").toInt(), 409);
  auto projection = body(); projection["operation"] = "projection"; projection["projection"] = "orthographic";
  QCOMPARE(agentRequest(info, route, projection).value("http_status").toInt(), 202);
  QTRY_COMPARE(agentRequest(info, "/v1/requests/" + projection.value("request_id").toString()).value("state").toString(), QString("succeeded"));
  QCOMPARE(controller->camera().value("projection").toString(), QString("orthographic"));
  auto invalid = body(); invalid["camera"] = controller->camera();
  auto badCamera = invalid.value("camera").toObject(); badCamera["distance"] = -1; invalid["camera"] = badCamera;
  QCOMPARE(agentRequest(info, route, invalid).value("http_status").toInt(), 202);
  QTRY_COMPARE(agentRequest(info, "/v1/requests/" + invalid.value("request_id").toString()).value("state").toString(), QString("failed"));
  QTimer pump;
  connect(&pump, &QTimer::timeout, window, [this] { window->qglview->grabFramebuffer(); });
  pump.start(30);
  const auto capture = body();
  QCOMPARE(agentRequest(info, "/v1/capture", capture).value("http_status").toInt(), 202);
  QJsonObject receipt;
  QTRY_COMPARE((receipt = agentRequest(info, "/v1/requests/" + capture.value("request_id").toString())).value("state").toString(), QString("succeeded"));
  const auto image = receipt.value("result").toObject();
  const auto png = agentImage(info, image.value("image_route").toString());
  QCOMPARE(QString::fromLatin1(AgentAnnotations::Store::hash(png)), image.value("png_sha256").toString());
  QCOMPARE(image.value("view_revision"), capture.value("expected_view_revision"));
  QCOMPARE(image.value("displayed_result_id"), capture.value("expected_result_id"));
  QFile evidence(QFileInfo(qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_LOG")).dir().filePath("agent-capture.png"));
  QVERIFY(evidence.open(QIODevice::WriteOnly)); evidence.write(png); evidence.close();
  controller->startDrawing();
  QCOMPARE(agentRequest(info, "/v1/capture", body()).value("http_status").toInt(), 409);
  QCOMPARE(agentRequest(info, "/v1/annotations?document_id=" + editor->annotations->sessionId).value("http_status").toInt(), 200);
  controller->cancelDrawing();
  QCOMPARE(editor->toPlainText(), QString("difference(){cube(10,center=true); cylinder(h=20,r=3,center=true);}"));
  bridge->stop();
}

void TestAnnotations::modelInspection()
{
  window->designActionAutoReload->setChecked(false);
  window->tabManager->actionNew();
  auto editor = window->activeEditor;
  editor->setPlainText("module part(s) { translate([s,0,0]) cube(s); }\n"
    "!difference() { for (s=[2:4]) part(s); #sphere(1); %cube(8); } *cube(99);\n");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  auto bridge = AgentAnnotations::Bridge::instance();
  bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  const auto info = bridge->connection();
  const auto result = window->annotationController->status().value("result").toObject();
  const auto treeId = result.value("tree_id").toString();
  QVERIFY(!treeId.isEmpty());
  auto tree = agentRequest(info, "/v1/trees/" + treeId);
  QCOMPARE(tree.value("kind").toString(), QString("evaluated"));
  QVERIFY(tree.value("complete").toBool());
  QVERIFY(tree.value("effective_root_ids") != tree.value("root_ids"));
  auto detached = AgentAnnotations::inspectModel(window->absoluteRootNode.get(), window->rootNode.get(), window->rootFile.get(), result);
  int cubes = 0;
  QSet<QString> spans;
  for (const auto& node : detached.nodes) {
    if (node.value("kind") != "cube") continue;
    ++cubes;
    auto size = node.value("attributes").toObject().value("size").toArray();
    QVERIFY(size[0].toInt() != 99);
    if (size[0].toInt() <= 4) spans.insert(QString::fromUtf8(QJsonDocument(node.value("location").toObject()).toJson()));
  }
  QCOMPARE(cubes, 4);
  QCOMPARE(spans.size(), 1); // Loop expansions share a source call, never a public node ID.
  const auto oldNode = agentRequest(info, "/v1/trees/" + treeId + "/nodes/1");
  editor->setPlainText("cube(7);");
  QVERIFY(agentRequest(info, "/v1/documents/" + editor->annotations->sessionId + "/parameters").value("stale_source").toBool());
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QVERIFY(window->annotationController->status().value("result").toObject().value("tree_id") != treeId);
  QCOMPARE(agentRequest(info, "/v1/trees/" + treeId + "/nodes/1"), oldNode);
  editor->setPlainText("");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  auto empty = window->annotationController->status().value("result").toObject();
  QVERIFY(agentRequest(info, "/v1/trees/" + empty.value("tree_id").toString()).value("empty").toBool());
  GroupNode wide(nullptr);
  for (int i = 0; i < 13000; ++i) wide.children.push_back(std::make_shared<CubeNode>(nullptr));
  auto bounded = AgentAnnotations::inspectModel(&wide, &wide, nullptr, empty);
  QCOMPARE(bounded.nodes.size(), 12000);
  QVERIFY(!bounded.metadata.value("complete").toBool());
  for (const auto& node : bounded.nodes)
    for (const auto& child : node.value("children").toArray())
      QVERIFY(bounded.nodes.contains(child.toObject().value("node_id").toString()));
  bridge->stop();
}

void TestAnnotations::sourceInspection()
{
  window->designActionAutoReload->setChecked(false);
  QTemporaryDir dir;
  QFile file(dir.filePath(QString::fromUtf8("pi\xc3\xa8" "ce.scad")));
  const QByteArray disk = "\xef\xbb\xbf// \xe2\x86\x92\r\necho(42); cube(2);\r\n";
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(disk);
  file.close();
  window->tabManager->open(file.fileName());
  auto editor = window->activeEditor;
  editor->setPlainText("echo(43); cube(3);\n");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  const auto camera = window->annotationController->camera();
  const auto before = window->annotationController->status().value("result");
  const auto dirty = editor->isContentModified(), undo = editor->canUndo();
  const auto revision = editor->sourceRevision;
  auto bridge = AgentAnnotations::Bridge::instance();
  bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  const auto info = bridge->connection();
  const auto document = editor->annotations->sessionId;
  const auto route = "/v1/documents/" + document;
  QJsonObject body{{"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
    {"server_id", info.value("server_id")}, {"document_id", document},
    {"window_id", window->annotationController->windowId}, {"origin", "disk"}};
  QCOMPARE(agentRequest(info, route + "/source-snapshots", body).value("http_status").toInt(), 202);
  QJsonObject job;
  QTRY_COMPARE((job = agentRequest(info, "/v1/requests/" + body.value("request_id").toString())).value("state").toString(), QString("succeeded"));
  const auto meta = job.value("result").toObject();
  QCOMPARE(meta.value("disk_sha256").toString(), QString::fromLatin1(AgentAnnotations::Store::hash(disk)));
  auto snapshot = agentRequest(info, "/v1/source-snapshots/" + meta.value("snapshot_id").toString());
  QCOMPARE(snapshot.value("text").toString(), QString::fromUtf8("// \xe2\x86\x92\necho(42); cube(2);\n"));
  QVERIFY(snapshot.value("source_sha256") != snapshot.value("disk_sha256"));
  body["origin"] = "editor";
  body["expected_source_revision"] = QString::number(revision);
  body["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(agentRequest(info, route + "/source-snapshots", body).value("http_status").toInt(), 202);
  QTRY_COMPARE((job = agentRequest(info, "/v1/requests/" + body.value("request_id").toString())).value("state").toString(), QString("succeeded"));
  snapshot = agentRequest(info, "/v1/source-snapshots/" + job.value("result").toObject().value("snapshot_id").toString());
  QCOMPARE(snapshot.value("text").toString(), editor->toPlainText());
  QCOMPARE(editor->sourceRevision, revision);
  QCOMPARE(editor->isContentModified(), dirty);
  QCOMPARE(editor->canUndo(), undo);
  QCOMPARE(window->annotationController->camera(), camera);
  QCOMPARE(window->annotationController->status().value("result"), before);
  QVERIFY(!agentRequest(info, route + "/results").value("results").toArray().isEmpty());
  const auto results = agentRequest(info, route + "/results").value("results");
  QVERIFY(!agentRequest(info, route + "/diagnostics").value("diagnostics").toArray().isEmpty());
  editor->setPlainText("cube( ;");
  body["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(agentRequest(info, route + "/source-snapshots", body).value("http_status").toInt(), 202);
  QTRY_COMPARE(agentRequest(info, "/v1/requests/" + body.value("request_id").toString()).value("state").toString(), QString("failed"));
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QCOMPARE(agentRequest(info, route + "/results").value("results"), results);
  auto attempts = agentRequest(info, route + "/attempts").value("attempts").toArray();
  QCOMPARE(attempts.last().toObject().value("outcome").toString(), QString("failed"));
  // A known document source is required, even for disk origin; no arbitrary path browsing.
  body["document_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  body["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(agentRequest(info, "/v1/documents/" + body.value("document_id").toString() + "/source-snapshots", body).value("http_status").toInt(), 202);
  QTRY_COMPARE(agentRequest(info, "/v1/requests/" + body.value("request_id").toString()).value("state").toString(), QString("failed"));
  bridge->stop();
}

void TestAnnotations::agentAccessLifecycle()
{
  auto service = AgentAnnotations::Bridge::instance();
  service->stop();
  QVERIFY(!QSettings().value("agentServer/enabled", false).toBool());
  service->showAccess(window);
  auto dialog = window->findChild<QDialog *>("agentAccessDialog");
  QVERIFY(dialog);
  auto enable = dialog->findChild<QPushButton *>("enableAgentAccess");
  auto stop = dialog->findChild<QPushButton *>("stopAgentAccess");
  QVERIFY(enable && stop);
  QVERIFY(enable->isEnabled());
  QVERIFY(!stop->isEnabled());
  enable->click();
  QTRY_COMPARE(service->connection().value("api_version").toInt(), 1);
  const auto firstId = service->connection().value("server_id");
  QVERIFY(QSettings().value("agentServer/enabled", false).toBool());
  QVERIFY(!enable->isEnabled());
  service->start();
  QCOMPARE(service->connection().value("server_id"), firstId);
  service->stop(false);
  QVERIFY(QSettings().value("agentServer/enabled", false).toBool());
  service->start(false);
  QTRY_COMPARE(service->connection().value("api_version").toInt(), 1);
  QVERIFY(service->connection().value("server_id") != firstId);
  stop->click();
  QVERIFY(service->connection().isEmpty());
  QVERIFY(!QSettings().value("agentServer/enabled", false).toBool());
  QVERIFY(enable->isEnabled());
  QVERIFY(!stop->isEnabled());
  dialog->close();
}

void TestAnnotations::targetInspection()
{
  QTemporaryDir dir;
  const auto path = dir.filePath("same.scad");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("cube(1);\n");
  file.close();
  window->designActionAutoReload->setChecked(false);
  window->tabManager->open(path);
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  const auto first = window->activeEditor->annotations->sessionId;
  auto second = std::make_unique<MainWindow>(QStringList{path});
  second->designActionAutoReload->setChecked(false);
  const auto other = second->activeEditor->annotations->sessionId;
  QVERIFY(first != other);
  QVERIFY(window->annotationController->windowId != second->annotationController->windowId);
  window->tabManager->actionNew();
  const auto untitled = window->activeEditor->annotations->sessionId;
  QVERIFY(first != untitled);
  QCOMPARE(window->annotationController->status().value("active_document_id").toString(), untitled);
  QCOMPARE(window->annotationController->status().value("document_id").toString(), first);
  QString error;
  auto bridge = AgentAnnotations::Bridge::instance();
  QVERIFY(bridge->snapshotSource(window->activeEditor, {{"origin", "disk"}}, &error).isEmpty());
  QVERIFY(error.contains("Untitled"));
  bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  auto info = bridge->connection();
  const auto route = "/v1/documents/" + other + "/source-snapshots";
  QJsonObject body{{"origin", "disk"}, {"document_id", other},
    {"window_id", second->annotationController->windowId}, {"server_id", info.value("server_id")},
    {"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}};
  QCOMPARE(agentRequest(info, route, body).value("http_status").toInt(), 202);
  QJsonObject job;
  QTRY_COMPARE((job = agentRequest(info, "/v1/requests/" + body.value("request_id").toString())).value("state").toString(), QString("succeeded"));
  const auto snapshotRoute = "/v1/source-snapshots/" + job.value("result").toObject().value("snapshot_id").toString();
  second.reset();
  bridge->publish();
  QTRY_COMPARE(agentRequest(info, snapshotRoute).value("http_status").toInt(), 410);
  body["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(agentRequest(info, route, body).value("http_status").toInt(), 202);
  QTRY_COMPARE(agentRequest(info, "/v1/requests/" + body.value("request_id").toString()).value("state").toString(), QString("failed"));
  window->tabManager->closeCurrentTab();
  bridge->stop();
}

void TestAnnotations::syntaxInspection()
{
  QTemporaryDir dir;
  auto write = [](const QString& path, const QByteArray& data) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
  };
  QVERIFY(write(dir.filePath("included.scad"), "included=7;\n"));
  QVERIFY(write(dir.filePath("library.scad"), "module part(x=1){cube(x);}\n"));
  const QString code = QString::fromUtf8("include <included.scad>\nuse <library.scad>\n"
    "// \xe2\x86\x92\nx=1; x=2; function f(a=3)=a>0 ? a+1 : -a;\n"
    "module item(s=4){if(s>1) translate([1,2,3]) cube(s); else sphere(1);}\n"
    "for(i=[0:2]) item(i); *cube(9); v=[for(i=[1:3]) if(i>1) i*2];\n"
    "echo(\"do not evaluate\"); import(str(\"file\",\".stl\"));\n");
  const auto path = dir.filePath("root.scad");
  QVERIFY(write(path, code.toUtf8()));
  window->tabManager->open(path);
  auto editor = window->activeEditor;
  // The live result is deliberately different from the source being parsed.
  editor->setPlainText("cube(4);\n");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  const auto root = window->rootFile;
  const auto model = window->absoluteRootNode;
  const auto view = window->annotationController->status().value("result");
  const auto camera = window->annotationController->camera();
  const auto handler = outputhandler;
  const auto handlerData = outputhandler_data;
  const auto oldError = parser_error_pos;
  QJsonObject source{{"document_id", editor->annotations->sessionId}, {"snapshot_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"path", path}, {"origin", "disk"}};
  auto inspect = [&](const QString& text) {
    const GuiLocker lock;
    return AgentAnnotations::inspectSyntax(source, text);
  };
  auto parsed = inspect(code);
  QVERIFY2(parsed.outcome.value("success").toBool(), QJsonDocument(parsed.outcome).toJson().constData());
  QCOMPARE(parsed.outcome.value("dependencies").toArray().size(), 3);
  QSet<QString> kinds, names;
  bool disabled = false;
  int assignments = 0;
  for (const auto& node : parsed.tree.nodes) {
    kinds.insert(node.value("kind").toString()); names.insert(node.value("name").toString());
    disabled |= node.value("attributes").toObject().value("disabled").toBool();
    if (node.value("kind") == "assignment" && node.value("name") == "x" &&
        node.value("location").toObject().value("path") == path) ++assignments;
    if (node.value("kind") == "literal" && node.value("attributes").toObject().value("value") == 2) {
      QVERIFY(node.value("location").isObject());
      QCOMPARE(node.value("location").toObject().value("column_units").toString(), QString("utf8_bytes"));
    }
  }
  QVERIFY(kinds.contains("module_definition") && kinds.contains("function_definition") && kinds.contains("binary") && kinds.contains("conditional") && kinds.contains("comprehension_for"));
  QVERIFY(names.contains("part") && names.contains("included"));
  QVERIFY(disabled);
  QCOMPARE(assignments, 2);
  const auto unicodeText = QString::fromUtf8("echo(\"\xe2\x86\x92\"); cube(1);");
  auto unicode = inspect(unicodeText);
  QVERIFY(unicode.outcome.value("success").toBool());
  for (const auto& node : unicode.tree.nodes) if (node.value("name") == "cube") {
    const auto location = node.value("location").toObject();
    const auto start = unicodeText.toUtf8().indexOf("cube");
    QCOMPARE(location.value("first_column").toInt(), int(start + 1));
    QCOMPARE(location.value("last_column").toInt(), int(unicodeText.toUtf8().indexOf(");", start) + 2));
  }
  for (const auto& invalid : {"cube( ;", "module broken(a=1) { cube(1);", "if(true) {sphere(1);", "cube([1,2,);"}) {
    for (int n = 0; n < 30; ++n) {
      auto failed = inspect(invalid);
      QVERIFY(!failed.outcome.value("success").toBool());
      QVERIFY(failed.tree.metadata.isEmpty());
      QVERIFY(!failed.outcome.value("diagnostics").toArray().isEmpty());
    }
  }
  auto deep = inspect(QString(300, '(') + "1" + QString(300, ')') + ";");
  QVERIFY(!deep.outcome.value("success").toBool());
  auto large = inspect(QString("cube(1);\n").repeated(2000));
  QVERIFY(!large.outcome.value("success").toBool());
  QVERIFY(large.outcome.value("limits_reached").toBool());
  auto wide = inspect(QString("cube(1);\n").repeated(600));
  QVERIFY(wide.outcome.value("success").toBool());
  const auto pages = AgentAnnotations::treePages(wide.tree);
  const auto wideBase = "/v1/trees/" + wide.tree.metadata.value("tree_id").toString();
  const auto wideRoot = wide.tree.nodes.value(wide.tree.metadata.value("root_ids").toArray().first().toString());
  const auto scopeId = wideRoot.value("children").toArray().first().toObject().value("node_id").toString();
  const auto page = pages.value(wideBase + "/nodes/" + scopeId + "/children");
  QCOMPARE(page.value("children").toArray().size(), 500);
  QCOMPARE(pages.value(page.value("next_cursor").toString()).value("children").toArray().size(), 100);
  {
    const GuiLocker lock;
    ParserInspection probe;
    probe.read = [](const std::string&) -> std::string { throw std::runtime_error("read exception probe"); };
    probe.dependency = [](const std::string&, const std::string&, const std::string&, const Location&) {};
    probe.alive = [] { return true; };
    SourceFile *file = nullptr;
    bool caught = false;
    try { parseInspection(file, "include <included.scad>", path.toStdString(), probe); }
    catch (const std::runtime_error&) { caught = true; }
    std::unique_ptr<SourceFile> cleanup(file);
    QVERIFY(caught);
    QVERIFY(parserInspection == nullptr);
  }
  auto missing = inspect("include <absent.scad>\ncube(1);");
  QVERIFY(!missing.outcome.value("dependencies_complete").toBool());
  QVERIFY(write(dir.filePath("cycle.scad"), "include <cycle.scad>\ncube(1);"));
  auto cycle = inspect("include <cycle.scad>\n");
  QVERIFY(!cycle.outcome.value("dependencies_complete").toBool());
  QCOMPARE(window->rootFile, root);
  QCOMPARE(window->absoluteRootNode, model);
  QCOMPARE(window->annotationController->status().value("result"), view);
  QCOMPARE(window->annotationController->camera(), camera);
  QCOMPARE(editor->toPlainText(), QString("cube(4);\n"));
  QCOMPARE(outputhandler, handler);
  QCOMPARE(outputhandler_data, handlerData);
  QCOMPARE(parser_error_pos, oldError);
  QVERIFY(!GuiLocker::isLocked());
  auto bridge = AgentAnnotations::Bridge::instance();
  bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  QString error;
  auto snapshot = bridge->snapshotSource(editor, {{"origin", "disk"}}, &error);
  QVERIFY(error.isEmpty());
  const auto info = bridge->connection();
  QJsonObject body{{"server_id", info.value("server_id")}, {"document_id", editor->annotations->sessionId},
    {"window_id", window->annotationController->windowId}, {"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"snapshot_id", snapshot.value("snapshot_id")}};
  QCOMPARE(agentRequest(info, "/v1/documents/" + editor->annotations->sessionId + "/parse", body).value("http_status").toInt(), 202);
  QJsonObject job;
  QTRY_COMPARE((job = agentRequest(info, "/v1/requests/" + body.value("request_id").toString())).value("state").toString(), QString("succeeded"));
  QVERIFY(job.value("result").toObject().value("success").toBool());
  const auto treePath = "/v1/trees/" + job.value("result").toObject().value("tree_id").toString();
  auto tree = agentRequest(info, treePath);
  QCOMPARE(tree.value("kind").toString(), QString("syntax"));
  const auto nodePath = treePath + "/nodes/" + tree.value("root_ids").toArray().first().toString();
  QCOMPARE(agentRequest(info, nodePath).value("kind").toString(), QString("source_file"));
  QVERIFY(!agentRequest(info, nodePath + "/children").value("children").toArray().isEmpty());
  QCOMPARE(agentRequest(info, treePath + "/nodes/invalid").value("http_status").toInt(), 410);
  bridge->stop();
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QCOMPARE(window->annotationController->status().value("result").toObject().value("outcome").toString(), QString("installed"));
}

void TestAnnotations::documentLifecycle()
{
  QTemporaryDir dir;
  const auto source = dir.filePath("lifecycle.scad");
  QFile file(source);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("cube(1);\n");
  file.close();
  window->designActionAutoReload->setChecked(false);
  window->tabManager->open(source);
  auto editor = dynamic_cast<ScintillaEditor *>(window->activeEditor);
  QVERIFY(editor);
  editor->qsci->setReadOnly(false);
  QImage image(8, 8, QImage::Format_RGB32);
  image.fill(Qt::blue);
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  QJsonObject camera{{"translation", QJsonArray{0, 0, 0}},
                     {"rotation", QJsonArray{0, 0, 0}},
                     {"distance", 100},
                     {"fov", 45},
                     {"projection", "perspective"}};
  AgentAnnotations::Record record{
    {{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
     {"comment", "original"},
     {"created_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
     {"width", 8},
     {"height", 8},
     {"png_sha256", QString::fromLatin1(AgentAnnotations::Store::hash(png))},
     {"camera", camera},
     {"strokes", QJsonArray{}},
     {"result", QJsonObject{}}},
    png};
  QVERIFY(editor->annotations->add(record));
  QVERIFY(!editor->isContentModified());
  editor->setText("cube(2);\n");
  QVERIFY(editor->annotations->edit(record.id(), "edited"));
  editor->undo();
  QCOMPARE(editor->annotations->records()[0].metadata.value("comment").toString(), QString("original"));
  editor->undo();
  QCOMPARE(editor->toPlainText(), QString("cube(1);\n"));
  editor->undo();
  QVERIFY(editor->annotations->records().isEmpty());
  editor->redo();
  QCOMPARE(editor->annotations->records().size(), 1);
  const auto modified = QFileInfo(source).lastModified();
  QVERIFY(window->tabManager->save(editor));
  QCOMPARE(QFileInfo(source).lastModified(), modified);
  QVERIFY(QFile::exists(source + ".annotations"));
  QVERIFY(editor->annotations->edit(record.id(), "keep through reload"));
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("sphere(3);\n");
  file.close();
  QVERIFY(window->tabManager->refreshDocument());
  QCOMPARE(editor->toPlainText(), QString("sphere(3);\n"));
  QVERIFY(!editor->canUndo());
  QVERIFY(editor->annotations->dirty());
  QCOMPARE(editor->annotations->records()[0].metadata.value("comment").toString(),
           QString("keep through reload"));
  const auto copyOnly = dir.filePath("copy-only.scad");
  QTimer chooser, deadline;
  chooser.setInterval(20);
  deadline.setSingleShot(true);
  deadline.setInterval(3000);
  connect(&chooser, &QTimer::timeout, window, [&] {
    if (auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
      dialog->selectFile(copyOnly);
      QMetaObject::invokeMethod(dialog, "accept");
    }
  });
  connect(&deadline, &QTimer::timeout, window, [] {
    if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
  });
  chooser.start();
  deadline.start();
  QVERIFY(window->tabManager->saveACopy(editor));
  chooser.stop();
  deadline.stop();
  QCOMPARE(editor->filepath, source);
  QVERIFY(editor->annotations->dirty());
  AgentAnnotations::Store copyStore;
  QVERIFY(copyStore.load(copyOnly + ".annotations"));
  QCOMPARE(copyStore.records()[0].png, png);
  const auto backupPath = qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_BACKUP");
  if (!backupPath.isEmpty()) {
    QTest::qWait(1600);
    auto backups = QDir(backupPath).entryList({"*.scad.annotations"}, QDir::Files, QDir::Time);
    QVERIFY(!backups.isEmpty());
    AgentAnnotations::Store backup;
    QVERIFY(backup.load(QDir(backupPath).filePath(backups.first())));
    QCOMPARE(backup.records()[0].metadata.value("comment").toString(), QString("keep through reload"));
    QVERIFY(editor->annotations->dirty());
  }
  const auto copy = dir.filePath("save-as.scad");
  QVERIFY(window->tabManager->saveAs(editor, copy));
  QVERIFY(!editor->annotations->dirty());
  AgentAnnotations::Store loaded;
  QVERIFY(loaded.load(copy + ".annotations"));
  QCOMPARE(loaded.records()[0].png, png);
  QVERIFY(editor->annotations->remove(record.id()));
  QVERIFY(window->tabManager->save(editor));
  QVERIFY(loaded.load(copy + ".annotations"));
  QVERIFY(loaded.records().isEmpty());
  editor->qsci->setReadOnly(true);
  QVERIFY(editor->annotations->add(record));
  editor->undoFromAnnotations(false);
  QVERIFY(editor->annotations->records().isEmpty());
  QVERIFY(editor->qsci->isReadOnly());
  QVERIFY(window->tabManager->save(editor));
}

void TestAnnotations::viewportWorkflow()
{
  window->designActionAutoReload->setChecked(false);
  auto editor = dynamic_cast<ScintillaEditor *>(window->activeEditor);
  QVERIFY(editor);
  editor->qsci->setReadOnly(false);
  editor->setPlainText(
    "difference(){cube([24,18,12],center=true); cylinder(h=20,r=4,center=true,$fn=48);}\n");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  auto controller = window->annotationController;
  QVERIFY(controller);
  controller->dock->show();
  window->resize(1200, 850);
  window->qglview->viewAll();
  // A hidden automated Windows test has no compositor swaps. Exercise actual GL painting.
  QTimer framePump;
  connect(&framePump, &QTimer::timeout, window, [this] { window->qglview->grabFramebuffer(); });
  framePump.start(30);
  QTest::qWait(300);
  QVERIFY(window->qglview->isVisible());
  QCOMPARE(controller->owner(), editor);
  const auto result = controller->status().value("result").toObject();
  QCOMPARE(result.value("outcome").toString(), QString("installed"));
  QVERIFY(!result.value("source_sha256").toString().isEmpty());
  controller->startDrawing();
  QVERIFY(controller->drafting());
  const QSize size = window->qglview->size();
  const QPoint start(size.width() / 3, size.height() / 2), end(2 * size.width() / 3, size.height() / 2);
  QTest::mousePress(window->qglview, Qt::LeftButton, Qt::NoModifier, start);
  QMouseEvent move(QEvent::MouseMove, QPointF(end), QPointF(window->qglview->mapToGlobal(end)),
                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(window->qglview, &move);
  QTest::mouseRelease(window->qglview, Qt::LeftButton, Qt::NoModifier, end);
  QVERIFY(window->qglview->annotationCameraLock.has_value());
  auto navigation=window->qglview->navigation();
  navigation->settings.animate=false;
  const auto lockedPose=navigation->current();
  window->qglview->navigationCube()->step(Eigen::Vector3d::UnitY(),90);
  QVERIFY(navigation->current()==lockedPose);
  QTest::keyClick(window->qglview,Qt::Key_Z,Qt::ControlModifier);
  QVERIFY(!window->qglview->annotationCameraLock.has_value());
  window->qglview->navigationCube()->step(Eigen::Vector3d::UnitZ(),15);
  QTest::qWait(50);
  QTest::keyClick(window->qglview,Qt::Key_Y,Qt::ControlModifier);
  QVERIFY(!window->qglview->annotationCameraLock.has_value());
  QTest::mousePress(window->qglview,Qt::LeftButton,Qt::NoModifier,start);
  QApplication::sendEvent(window->qglview,&move);
  QTest::mouseRelease(window->qglview,Qt::LeftButton,Qt::NoModifier,end);
  QVERIFY(window->qglview->annotationCameraLock.has_value());
  const QImage directAnnotated=window->qglview->grabFrame();
  const QImage directClean=window->qglview->grabFrame(QGLView::CaptureContent::ModelOnly);
  QVERIFY(directAnnotated!=directClean);
  const auto camera = controller->camera();
  window->qglview->cam.setVpd(window->qglview->cam.viewer_distance * 2);
  window->qglview->update();
  QTest::qWait(100);
  QCOMPARE(controller->camera(), camera);
  QTimer accept;
  accept.setInterval(20);
  connect(&accept, &QTimer::timeout, window, [] {
    auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog) return;
    if (auto comment = dialog->findChild<QPlainTextEdit *>("annotationComment")) {
      comment->setPlainText(QString::fromUtf8("Please widen this opening \xe2\x86\x92"));
      dialog->accept();
    }
  });
  accept.start();
  const auto count = editor->annotations->records().size();
  QTest::keyClick(window->qglview, Qt::Key_Return);
  QTRY_COMPARE_WITH_TIMEOUT(editor->annotations->records().size(), count + 1, 6000);
  accept.stop();
  QVERIFY(!controller->drafting());
  const auto record = editor->annotations->records().last();
  QCOMPARE(record.metadata.value("strokes").toArray().size(), 1);
  QCOMPARE(QImage::fromData(record.png).convertToFormat(directAnnotated.format()),directAnnotated);
  QVERIFY(!editor->isContentModified());
  QVERIFY(editor->annotations->dirty());
  const auto evidence = QFileInfo(qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_LOG")).dir();
  QFile image(evidence.filePath("annotation-capture.png"));
  QVERIFY(image.open(QIODevice::WriteOnly));
  image.write(record.png);
  image.close();
  QVERIFY(window->grab().save(evidence.filePath("annotation-window.png")));
  window->tabManager->createTab("");
  auto other = window->activeEditor;
  QVERIFY(other != editor);
  QCOMPARE(controller->owner(), editor);
  controller->dock->findChild<QLineEdit *>()->setFocus();
  QTest::qWait(20);
  QCOMPARE(controller->actionOwner(), editor);
  window->tabManager->undo();
  QCOMPARE(editor->annotations->records().size(), count);
  QVERIFY(other->toPlainText().isEmpty());
  window->tabManager->redo();
  QCOMPARE(editor->annotations->records().size(), count + 1);
  window->tabManager->open(editor->filepath);
  // Selection changes neither model nor camera; fresh capture never includes historical marks.
  controller->dock->findChild<QListWidget *>("annotationList")->setCurrentRow(0);
  QCOMPARE(controller->camera(), camera);
  bool complete = false;
  QString error;
  QByteArray fresh;
  controller->capture(result.value("result_id").toString(), [&](QJsonObject, QByteArray png, QString failure) {
    fresh = png;
    error = failure;
    complete = true;
  });
  QTRY_VERIFY_WITH_TIMEOUT(complete, 6000);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QVERIFY(fresh != record.png);
  QFile plain(evidence.filePath("annotation-fresh.png"));
  QVERIFY(plain.open(QIODevice::WriteOnly));
  plain.write(fresh);
  plain.close();
  // Back resumes the same empty draft; Cancel publishes nothing; empty Add is valid.
  int dialogChoice = 2;
  int dialogVisits = 0;
  QTimer dialogDriver;
  dialogDriver.setInterval(20);
  connect(&dialogDriver, &QTimer::timeout, window, [&] {
    auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog || !dialog->findChild<QPlainTextEdit *>("annotationComment")) return;
    ++dialogVisits;
    dialog->done(dialogChoice);
  });
  dialogDriver.start();
  controller->startDrawing();
  QTest::keyClick(window->qglview, Qt::Key_Return);
  QTRY_COMPARE_WITH_TIMEOUT(dialogVisits, 1, 6000);
  QVERIFY(controller->drafting());
  QCOMPARE(editor->annotations->records().size(), count + 1);
  dialogChoice = QDialog::Rejected;
  QTest::keyClick(window->qglview, Qt::Key_Return);
  QTRY_COMPARE_WITH_TIMEOUT(dialogVisits, 2, 6000);
  QVERIFY(!controller->drafting());
  QCOMPARE(editor->annotations->records().size(), count + 1);
  dialogChoice = QDialog::Accepted;
  controller->startDrawing();
  QTest::keyClick(window->qglview, Qt::Key_Return);
  QTRY_COMPARE_WITH_TIMEOUT(editor->annotations->records().size(), count + 2, 6000);
  dialogDriver.stop();
  const auto empty = editor->annotations->records().last();
  QVERIFY(empty.metadata.value("comment").toString().isEmpty());
  QVERIFY(empty.metadata.value("strokes").toArray().isEmpty());
  editor->annotations->remove(empty.id());
  controller->startDrawing();
  QTest::keyClick(window->qglview, Qt::Key_Escape);
  QVERIFY(!controller->drafting());
  controller->startDrawing();
  QTest::mouseClick(window->qglview, Qt::RightButton);
  QVERIFY(!controller->drafting());
  controller->startDrawing();
  QVERIFY(controller->drafting());
  window->resize(1150, 800);
  QTRY_VERIFY(!controller->drafting());
  QVERIFY(!GuiLocker::isLocked());
  editor->annotations->remove(record.id());
}

void TestAnnotations::agentAndAutomaticReload()
{
  QTemporaryDir dir;
  auto write = [](const QString& path, const QByteArray& text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
  };
  const auto source = dir.filePath("root.scad"), include = dir.filePath("size.scad"),
             library = dir.filePath("part.scad");
  const QByteArray initial = "include <size.scad>\nuse <part.scad>\npart(size);\n";
  QVERIFY(write(include, "size=10;\n"));
  QVERIFY(write(library, "module part(s){cube(s);}\n"));
  QVERIFY(write(source, initial));
  window->tabManager->open(source);
  auto editor = window->activeEditor;
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  window->qglview->viewAll();
  auto controller = window->annotationController;
  QCOMPARE(controller->owner(), editor);
  QTimer framePump;
  connect(&framePump, &QTimer::timeout, window, [this] { window->qglview->grabFramebuffer(); });
  framePump.start(30);
  auto bridge = AgentAnnotations::Bridge::instance();
  bridge->start(false);
  QTRY_VERIFY_WITH_TIMEOUT(!bridge->connection().isEmpty(), 3000);
  const auto info = bridge->connection();
  QNetworkAccessManager network;
  auto request = [&](const QString& route, const QJsonObject& body = QJsonObject{}) {
    QNetworkRequest request(QUrl(info.value("url").toString() + route));
    request.setRawHeader("X-OpenSCAD-Agent", "1");
    request.setRawHeader("X-OpenSCAD-Agent-Server", info.value("server_id").toString().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto reply =
      body.isEmpty() ? network.get(request) : network.post(request, QJsonDocument(body).toJson());
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(6000);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    loop.exec();
    auto result = QJsonDocument::fromJson(reply->readAll()).object();
    result["http_status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return result;
  };
  QCOMPARE(request("/v1/info").value("http_status").toInt(), 200);
  const auto current = controller->status();
  const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonObject body{{"server_id", info.value("server_id")},
                   {"request_id", requestId},
                   {"document_id", editor->annotations->sessionId},
                   {"window_id", controller->windowId},
                   {"viewport_id", controller->viewportId},
                   {"expected_result_id", current.value("displayed_result_id")},
                   {"expected_view_revision", current.value("view_revision")}};
  QCOMPARE(request("/v1/capture", body).value("http_status").toInt(), 202);
  QJsonObject job;
  QTRY_VERIFY_WITH_TIMEOUT(
    (job = request("/v1/requests/" + requestId)).value("state").toString() == "succeeded" ||
      job.value("state") == "failed", 6000);
  QCOMPARE(job.value("state").toString(), QString("succeeded"));
  auto metadata = job.value("result").toObject();
  auto png = agentImage(info, metadata.take("image_route").toString());
  metadata["comment"] = "Retain this unsaved evidence across external reload";
  QString error;
  QVERIFY2(editor->annotations->add({metadata, png}, &error), qPrintable(error));
  bridge->publish();
  const auto savedCamera = controller->camera();
  auto changedCamera = savedCamera;
  changedCamera["distance"] = savedCamera.value("distance").toDouble() * 1.5;
  auto cameraBody = body;
  cameraBody["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  cameraBody["camera"] = changedCamera;
  QCOMPARE(request("/v1/viewports/" + controller->viewportId + "/view", cameraBody)
             .value("http_status")
             .toInt(),
           202);
  QTRY_COMPARE(controller->camera(), changedCamera);
  cameraBody.remove("camera");
  cameraBody["expected_view_revision"] = controller->status().value("view_revision");
  cameraBody["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(request("/v1/annotations/" + metadata.value("id").toString() + "/restore", cameraBody)
             .value("http_status")
             .toInt(),
           202);
  QTRY_COMPARE(controller->camera(), savedCamera);
  QCOMPARE(editor->annotations->records()[0].png, png);
  QCOMPARE(window->activeEditor, editor);
  controller->startDrawing();
  QVERIFY(controller->drafting());
  // The GUI thread is blocked here; the worker must still serve accepted evidence.
  QTcpSocket socket;
  QUrl url(info.value("url").toString());
  socket.connectToHost("127.0.0.1", url.port());
  QVERIFY(socket.waitForConnected(2000));
  QByteArray header = "GET /v1/annotations?document_id=" + editor->annotations->sessionId.toUtf8() +
                      " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(url.port()) +
                      "\r\nX-OpenSCAD-Agent: 1\r\nX-OpenSCAD-Agent-Server: " +
                      info.value("server_id").toString().toUtf8() + "\r\n\r\n";
  socket.write(header);
  socket.flush();
  QVERIFY(socket.waitForReadyRead(2000));
  const auto response = socket.readAll();
  QVERIFY(response.startsWith("HTTP/1.1 200"));
  controller->cancelDrawing();
  const auto records = request("/v1/annotations?document_id=" + editor->annotations->sessionId)
                         .value("annotations")
                         .toArray();
  QCOMPARE(records.size(), 1);
  window->designActionAutoReload->setChecked(true);
  QTest::qWait(1100);  // Upstream include/use cache timestamps have one-second resolution.
  const auto rootHash = editor->sourceFingerprint;
  auto prior = controller->status().value("result").toObject().value("result_id");
  QVERIFY(write(include, "size=16;\n"));
  QTRY_VERIFY_WITH_TIMEOUT(controller->status().value("result").toObject().value("result_id") != prior, 8000);
  QCOMPARE(editor->sourceFingerprint, rootHash);
  QVERIFY(editor->annotations->dirty());
  QTest::qWait(1100);
  prior = controller->status().value("result").toObject().value("result_id");
  QVERIFY(write(library, "module part(s){sphere(r=s/2,$fn=32);}\n"));
  QTRY_VERIFY_WITH_TIMEOUT(controller->status().value("result").toObject().value("result_id") != prior, 8000);
  QCOMPARE(editor->sourceFingerprint, rootHash);
  const QByteArray changed = initial + "translate([20,0,0]) cube(3);\n";
  QVERIFY(write(source, changed));
  QTRY_COMPARE_WITH_TIMEOUT(editor->sourceFingerprint,
                            QString::fromLatin1(AgentAnnotations::Store::hash(changed)), 8000);
  QTRY_VERIFY_WITH_TIMEOUT(
    controller->status().value("result").toObject().value("source_sha256") == editor->sourceFingerprint,
    8000);
  QVERIFY(editor->annotations->dirty());
  QVERIFY(!editor->canUndo());
  QCOMPARE(editor->annotations->records()[0].png, png);
  QVERIFY(write(source, "cube( ;\n"));
  QTRY_COMPARE_WITH_TIMEOUT(
    controller->status().value("evaluation").toObject().value("outcome").toString(), QString("failed"),
    8000);
  window->designActionAutoReload->setChecked(false);
  bridge->stop(false);
  QVERIFY(bridge->connection().isEmpty());
  editor->annotations->remove(metadata.value("id").toString());
  editor->setPlainText("cube(4);\n");
  QVERIFY(QMetaObject::invokeMethod(window, "on_designActionRender_triggered"));
  QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 10000);
  QCOMPARE(controller->status().value("result").toObject().value("document_id").toString(),
           editor->annotations->sessionId);
  QCOMPARE(controller->status().value("evaluation").toObject().value("outcome").toString(),
           QString("installed"));
  // Publication can still have a pending layout/resize before the first paint.
  // Capture a settled viewport; resize cancellation is a separate guarded behavior.
  QSignalSpy firstFrame(window->qglview, &QGLView::frameRendered);
  window->qglview->update();
  QTRY_VERIFY_WITH_TIMEOUT(!firstFrame.isEmpty(), 6000);
  bool rendered = false;
  QString renderError;
  controller->capture(controller->status().value("result").toObject().value("result_id").toString(),
                      [&](QJsonObject, QByteArray image, QString failure) {
                        renderError = failure;
                        rendered = !image.isEmpty();
                      });
  QTRY_VERIFY_WITH_TIMEOUT(rendered || !renderError.isEmpty(), 6000);
  QVERIFY2(rendered, qPrintable(renderError));
  editor->setPlainText("");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QCOMPARE(controller->owner(), editor);
  QVERIFY(!controller->busy());
  QCOMPARE(controller->status().value("result").toObject().value("source_sha256").toString(),
           editor->sourceFingerprint);
}
