#include "TestAnnotations.h"
#include "gui/ScintillaEditor.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/annotations/AnnotationBridge.h"
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
  window->actionRenderPreview();
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
  controller->capture(result.value("id").toString(), [&](QJsonObject, QByteArray png, QString failure) {
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
  window->actionRenderPreview();
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
    request.setRawHeader("X-Agent-Annotations", "1");
    request.setRawHeader("X-Agent-Annotations-Server", info.value("server_id").toString().toUtf8());
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
                   {"expected_result_id", current.value("result").toObject().value("id").toString()}};
  const auto python = qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_PYTHON");
  const auto client = qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_CLIENT");
  if (!python.isEmpty() && !client.isEmpty()) {
    const auto connectionFile = dir.filePath("connection.json"),
               requestFile = dir.filePath("request.json"), image = dir.filePath("client.png");
    auto clientBody = body;
    clientBody["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(write(connectionFile, QJsonDocument(info).toJson()));
    QVERIFY(write(requestFile, QJsonDocument(clientBody).toJson()));
    QProcess process;
    process.start(python, {client, "--connection", connectionFile, "post", "/v1/capture", "--body",
                           requestFile, "--output", image});
    QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 12000);
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
    QVERIFY(!QImage(image).isNull());
  }
  QCOMPARE(request("/v1/capture", body).value("http_status").toInt(), 202);
  QJsonObject job;
  QTRY_VERIFY_WITH_TIMEOUT(
    (job = request("/v1/requests/" + requestId)).value("state").toString() != "queued", 6000);
  QCOMPARE(job.value("state").toString(), QString("succeeded"));
  auto metadata = job.value("result").toObject();
  auto png = QByteArray::fromBase64(metadata.take("png_base64").toString().toLatin1());
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
                      "\r\nX-Agent-Annotations: 1\r\nX-Agent-Annotations-Server: " +
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
  auto prior = controller->status().value("result").toObject().value("id");
  QVERIFY(write(include, "size=16;\n"));
  QTRY_VERIFY_WITH_TIMEOUT(controller->status().value("result").toObject().value("id") != prior, 8000);
  QCOMPARE(editor->sourceFingerprint, rootHash);
  QVERIFY(editor->annotations->dirty());
  QTest::qWait(1100);
  prior = controller->status().value("result").toObject().value("id");
  QVERIFY(write(library, "module part(s){sphere(r=s/2,$fn=32);}\n"));
  QTRY_VERIFY_WITH_TIMEOUT(controller->status().value("result").toObject().value("id") != prior, 8000);
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
  bool rendered = false;
  QString renderError;
  controller->capture(controller->status().value("result").toObject().value("id").toString(),
                      [&](QJsonObject, QByteArray image, QString failure) {
                        renderError = failure;
                        rendered = !image.isEmpty();
                      });
  QTRY_VERIFY_WITH_TIMEOUT(rendered || !renderError.isEmpty(), 6000);
  QVERIFY2(rendered, qPrintable(renderError));
  editor->setPlainText("");
  window->actionRenderPreview();
  QCOMPARE(controller->owner(), editor);
  QVERIFY(!controller->busy());
  QCOMPARE(controller->status().value("result").toObject().value("source_sha256").toString(),
           editor->sourceFingerprint);
}
