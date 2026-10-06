#include "TestSourceTransactions.h"
#include "gui/ScintillaEditor.h"
#include "gui/SavedViews.h"
#include "gui/CameraNavigation.h"
#include "gui/annotations/AnnotationBridge.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/source/SourceEditTransaction.h"
#include "gui/source/DiskSourceState.h"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

namespace {
QByteArray read(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}
bool write(const QString& path, const QByteArray& text) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size();
}
AgentAnnotations::Record annotation() {
  QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::blue);
  QByteArray png;
  QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
  return {{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"comment", "source transaction"},
    {"created_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    {"width", 8}, {"height", 8}, {"png_sha256", QString::fromLatin1(AgentAnnotations::Store::hash(png))},
    {"camera", QJsonObject{{"translation", QJsonArray{0,0,0}}, {"rotation", QJsonArray{0,0,0}},
      {"distance",100}, {"fov",45}, {"projection","perspective"}}},
    {"strokes", QJsonArray{}}, {"result", QJsonObject{}}}, png};
}
QJsonObject http(const QJsonObject& info, const QString& route, const QJsonObject& body = {}) {
  QNetworkAccessManager network;
  QNetworkRequest request(QUrl(info.value("url").toString() + route));
  request.setRawHeader("X-OpenSCAD-Agent", "1");
  request.setRawHeader("X-OpenSCAD-Agent-Server", info.value("server_id").toString().toUtf8());
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  auto reply = body.isEmpty() ? network.get(request) : network.post(request, QJsonDocument(body).toJson());
  QEventLoop loop;
  QTimer timeout; timeout.setSingleShot(true); timeout.start(6000);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  loop.exec();
  auto result = QJsonDocument::fromJson(reply->readAll()).object();
  result["http_status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  return result;
}
struct DialogChoice {
  QTimer poll, deadline;
  bool seen = false, timedOut = false;
  DialogChoice(const std::function<bool(QWidget *)>& choose) {
    poll.setInterval(20); deadline.setSingleShot(true); deadline.setInterval(4000);
    QObject::connect(&poll, &QTimer::timeout, [this, choose] {
      if (auto dialog = QApplication::activeModalWidget()) {
        if (choose(dialog)) { seen = true; poll.stop(); deadline.stop(); }
      }
    });
    QObject::connect(&deadline, &QTimer::timeout, [this] {
      timedOut = true; poll.stop();
      if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    poll.start(); deadline.start();
  }
};
}

void TestSourceTransactions::capturedDocumentUndoAndHttp()
{
  window->designActionAutoReload->setChecked(false);
  QTemporaryDir dir;
  const auto path = dir.filePath("captured.scad");
  QVERIFY(write(path, "cube(1);\n"));
  window->tabManager->open(path);
  auto editor = dynamic_cast<ScintillaEditor *>(window->activeEditor);
  QVERIFY(editor);
  const auto original = editor->toPlainText();
  editor->qsci->setCursorPosition(1, 0);
  QTest::keyClicks(editor->qsci, " ");
  const auto typed = editor->toPlainText();
  QVERIFY(typed != original);
  QVERIFY(editor->annotations->add(annotation()));
  const auto bytes = typed.toUtf8();
  const auto offset = bytes.indexOf('1');
  SourceEditing::EditRequest request{editor->documentId(), editor->sourceRevision, editor->sourceFingerprint,
    {{offset, offset + 1, "1", "20"}}};
  const auto capturedId = editor->documentId();
  const auto oldRevision = editor->sourceRevision;
  window->tabManager->actionNew();
  auto other = window->activeEditor;
  other->setPlainText("sphere(3);\n");
  QVERIFY(other != editor);
  QCOMPARE(SourceEditing::apply(*editor, request).error, SourceEditing::EditError::None);
  QCOMPARE(other->toPlainText(), QString("sphere(3);\n"));
  const auto changed = editor->toPlainText();
  QVERIFY(changed.contains("cube(20)"));
  editor->undo(); QCOMPARE(editor->toPlainText(), typed);
  QCOMPARE(editor->annotations->records().size(), 1);
  editor->undo(); QVERIFY(editor->annotations->records().isEmpty());
  QCOMPARE(editor->toPlainText(), typed);
  editor->undo(); QCOMPARE(editor->toPlainText(), original);
  editor->redo(); QCOMPARE(editor->toPlainText(), typed);
  editor->redo(); QCOMPARE(editor->annotations->records().size(), 1);
  editor->redo(); QCOMPARE(editor->toPlainText(), changed);
  QVERIFY(editor->sourceRevision > oldRevision);
  QCOMPARE(editor->documentId(), capturedId);
  auto bridge = AgentAnnotations::Bridge::instance(); bridge->start(false);
  QTRY_COMPARE(bridge->connection().value("api_version").toInt(), 1);
  const auto info = bridge->connection();
  QJsonObject body{{"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
    {"server_id", info.value("server_id")}, {"document_id", capturedId},
    {"window_id", window->annotationController->windowId}, {"origin", "editor"},
    {"expected_source_revision", QString::number(editor->sourceRevision)}};
  const auto route = "/v1/documents/" + capturedId + "/source-snapshots";
  QCOMPARE(http(info, route, body).value("http_status").toInt(), 202);
  QJsonObject job;
  QTRY_COMPARE((job = http(info, "/v1/requests/" + body.value("request_id").toString())).value("state").toString(), QString("succeeded"));
  auto snapshot = http(info, "/v1/source-snapshots/" + job.value("result").toObject().value("snapshot_id").toString());
  QCOMPARE(snapshot.value("text").toString(), changed);
  QCOMPARE(snapshot.value("source_revision").toString(), QString::number(editor->sourceRevision));
  QCOMPARE(snapshot.value("source_sha256").toString(), editor->sourceFingerprint);
  body["expected_source_revision"] = QString::number(oldRevision);
  body["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(http(info, route, body).value("http_status").toInt(), 202);
  QTRY_COMPARE(http(info, "/v1/requests/" + body.value("request_id").toString()).value("state").toString(), QString("failed"));
  bridge->stop();
  window->tabManager->open(path);
  QVERIFY(window->tabManager->save(editor));
  QPointer<EditorInterface> captured(editor);
  window->tabManager->closeCurrentTab();
  QTRY_VERIFY(captured.isNull());
  // A caller must reject this null captured pointer; it must never fall through to the active editor.
  QVERIFY(window->activeEditor != captured.data());
  QCOMPARE(other->toPlainText(), QString("sphere(3);\n"));
  other->setContentModified(false);
  window->tabManager->closeCurrentTab();
}

void TestSourceTransactions::conflictsCancelSaveAsCopyAndPersistence()
{
  QTemporaryDir dir;
  const auto path = dir.filePath(QString::fromUtf8("\xc3\xa9-\xf0\x9f\x98\x80.scad"));
  QVERIFY(write(path, "cube(1);\n"));
  window->tabManager->open(path);
  auto editor = window->activeEditor;
  editor->insertOrReplaceText("// unsaved\n");
  QVERIFY(editor->annotations->add(annotation()));
  auto views = SavedViews::forEditor(editor);
  QVERIFY(!views->add(window->qglview->navigation()->current()).isEmpty());
  const auto buffer = editor->toPlainText();
  const auto baseline = *editor->sourceDiskBaseline;
  const auto timestamp = QFileInfo(path).lastModified();
  QVERIFY(write(path, "cube(2);\n"));
  QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
  QVERIFY(file.setFileTime(timestamp, QFileDevice::FileModificationTime)); file.close();
  DialogChoice cancel([](QWidget *widget) {
    auto box = qobject_cast<QMessageBox *>(widget);
    if (!box || box->objectName() != "sourceSaveConflict") return false;
    box->button(QMessageBox::Cancel)->click(); return true;
  });
  QVERIFY(!window->tabManager->save(editor));
  QVERIFY(cancel.seen && !cancel.timedOut);
  QCOMPARE(read(path), QByteArray("cube(2);\n"));
  QCOMPARE(editor->toPlainText(), buffer);
  QVERIFY(editor->isContentModified());
  QVERIFY(editor->sourceDiskBaseline->matches(baseline));
  QVERIFY(!QFile::exists(path + ".annotations"));
  QVERIFY(!QFile::exists(SavedViews::companion(path)));
  const auto destination = dir.filePath("save-as.scad");
  bool saveAsChosen = false;
  DialogChoice chooseSaveAs([&](QWidget *widget) {
    if (auto box = qobject_cast<QMessageBox *>(widget)) {
      if (box->objectName() != "sourceSaveConflict" || saveAsChosen) return false;
      saveAsChosen = true;
      for (auto button : box->buttons())
        if (box->buttonRole(button) == QMessageBox::ActionRole) { button->click(); break; }
      return false;
    }
    if (auto dialog = qobject_cast<QFileDialog *>(widget)) {
      // selectFile() can retain the current selection while the visible filename
      // editor has focus. Drive that editor and verify the actual save target.
      auto filename = dialog->findChild<QLineEdit *>("fileNameEdit");
      if (!filename) return false;
      filename->setText(destination);
      if (dialog->selectedFiles() != QStringList{destination}) return false;
      QMetaObject::invokeMethod(dialog, "accept");
      return true;
    }
    return false;
  });
  QVERIFY(window->tabManager->save(editor));
  QVERIFY(saveAsChosen && chooseSaveAs.seen && !chooseSaveAs.timedOut);
  QCOMPARE(editor->filepath, destination);
  QVERIFY(editor->sourceDiskBaseline->matches(SourceEditing::DiskSourceState::capture(destination)));
  QVERIFY(!editor->isContentModified()); QVERIFY(!editor->annotations->dirty()); QVERIFY(!views->dirty());
  AgentAnnotations::Store stored;
  QVERIFY(stored.load(destination + ".annotations")); QCOMPARE(stored.records().size(), 1);
  SavedViews storedViews; QVERIFY(storedViews.load(destination)); QCOMPARE(storedViews.entries().size(), 1);
  editor->insertOrReplaceText("// copy edit\n");
  const auto identity = editor->documentId();
  const auto revision = editor->sourceRevision;
  const auto savedBaseline = *editor->sourceDiskBaseline;
  const auto copy = dir.filePath("copy.scad");
  QVERIFY(window->tabManager->saveACopy(editor, copy));
  QCOMPARE(editor->filepath, destination); QCOMPARE(editor->documentId(), identity);
  QCOMPARE(editor->sourceRevision, revision); QVERIFY(editor->isContentModified());
  QVERIFY(editor->sourceDiskBaseline->matches(savedBaseline));
  QVERIFY(write(copy, "another owner"));
  DialogChoice rejectCopy([](QWidget *widget) {
    auto box = qobject_cast<QMessageBox *>(widget);
    if (!box || box->objectName() != "sourceSaveConflict") return false;
    box->button(QMessageBox::Cancel)->click(); return true;
  });
  QVERIFY(!window->tabManager->saveACopy(editor, copy));
  QVERIFY(rejectCopy.seen && !rejectCopy.timedOut); QCOMPARE(read(copy), QByteArray("another owner"));
  QVERIFY(window->tabManager->save(editor));
  window->tabManager->closeCurrentTab();
}

void TestSourceTransactions::companionFailureReportsSavedSource()
{
  QTemporaryDir dir;
  const auto path = dir.filePath("partial.scad");
  QVERIFY(write(path, "old"));
  SavedViews views;
  QVERIFY(views.load(path));
  QVERIFY(!views.add(window->qglview->navigation()->current()).isEmpty());
  const auto approved = SourceEditing::DiskSourceState::capture(path);
  QString error;
  bool sourceSaved = false;
  QVERIFY(!views.save(path, false, [&] {
    sourceSaved = SourceEditing::writeSource(path, "new", approved).saved;
    // An actual filesystem failure after source success, confined to this temporary fixture.
    QDir().mkdir(SavedViews::companion(path));
    return sourceSaved;
  }, error));
  QVERIFY(sourceSaved); QCOMPARE(read(path), QByteArray("new"));
  QVERIFY(views.dirty()); QVERIFY(error.contains("source was saved"));
}

void TestSourceTransactions::sourceDecodingPreservesExistingPolicy()
{
  QTemporaryDir dir;
  const auto path = dir.filePath("bom.scad");
  const auto raw = QByteArray::fromHex("efbbbf") + "// Unicode \xc3\xa9\r\ncube(1);\r\n";
  QVERIFY(write(path, raw));
  window->tabManager->open(path);
  auto editor = window->activeEditor;
  QCOMPARE(editor->toPlainText(), QString::fromUtf8("// Unicode \xc3\xa9\ncube(1);\n"));
  QCOMPARE(editor->sourceDiskBaseline->sha256, QCryptographicHash::hash(raw, QCryptographicHash::Sha256));
  QVERIFY(!editor->isContentModified());
  window->tabManager->closeCurrentTab();
}

void TestSourceTransactions::overwriteApprovalIsBoundToObservedVersion()
{
  QTemporaryDir dir;
  const auto path = dir.filePath("overwrite.scad");
  QVERIFY(write(path, "initial"));
  window->tabManager->open(path);
  auto editor = window->activeEditor;
  editor->insertOrReplaceText("edit");
  QVERIFY(write(path, "external"));
  bool approved = false;
  DialogChoice race([&](QWidget *widget) {
    auto box = qobject_cast<QMessageBox *>(widget);
    if (!box) return false;
    if (box->objectName() == "sourceSaveConflict" && !approved) {
      approved = true;
      write(path, "changed during approval");
      for (auto button : box->buttons())
        if (box->buttonRole(button) == QMessageBox::DestructiveRole) { button->click(); break; }
      return false;
    }
    if (box->windowTitle() == "Source was not saved") { box->accept(); return true; }
    return false;
  });
  QVERIFY(!window->tabManager->save(editor));
  QVERIFY(approved && race.seen && !race.timedOut);
  QCOMPARE(read(path), QByteArray("changed during approval"));
  QVERIFY(editor->isContentModified());
  DialogChoice overwrite([](QWidget *widget) {
    auto box = qobject_cast<QMessageBox *>(widget);
    if (!box || box->objectName() != "sourceSaveConflict") return false;
    for (auto button : box->buttons())
      if (box->buttonRole(button) == QMessageBox::DestructiveRole) { button->click(); return true; }
    return false;
  });
  QVERIFY(window->tabManager->save(editor));
  QVERIFY(overwrite.seen && !overwrite.timedOut);
  QVERIFY(!editor->isContentModified());
  QVERIFY(editor->sourceDiskBaseline->matches(SourceEditing::DiskSourceState::capture(path)));
  window->tabManager->closeCurrentTab();
}

void TestSourceTransactions::annotationFailureReportsSavedSource()
{
  QTemporaryDir dir;
  const auto path = dir.filePath("annotation-partial.scad");
  QVERIFY(write(path, "cube(1);\n"));
  window->tabManager->open(path);
  auto editor = window->activeEditor;
  editor->insertOrReplaceText("// source success\n");
  QVERIFY(editor->annotations->add(annotation()));
  bool injected = false;
  QObject connectionContext;
  const auto connection = connect(editor, &EditorInterface::modificationChanged, &connectionContext, [&] {
    if (!injected && !editor->isContentModified()) {
      injected = QDir().mkdir(path + ".annotations");
    }
  });
  QString partialMessage;
  DialogChoice dismiss([&](QWidget *widget) {
    auto box = qobject_cast<QMessageBox *>(widget);
    if (!box || box->windowTitle() != "Annotations not saved") return false;
    partialMessage = box->text(); box->accept(); return true;
  });
  QVERIFY(!window->tabManager->save(editor));
  disconnect(connection);
  QVERIFY(injected && dismiss.seen && !dismiss.timedOut);
  QVERIFY(partialMessage.contains("source file was saved"));
  QVERIFY(!editor->isContentModified()); QVERIFY(editor->annotations->dirty());
  QVERIFY(editor->sourceDiskBaseline->matches(SourceEditing::DiskSourceState::capture(path)));
  QVERIFY(read(path).contains("source success"));
  // Repair only this temporary fixture so subsequent ordinary native suites see a clean tab.
  QVERIFY(QDir().rmdir(path + ".annotations"));
  QVERIFY(window->tabManager->save(editor));
  window->tabManager->closeCurrentTab();
}
