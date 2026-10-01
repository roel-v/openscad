#include "AnnotationBridge.h"
#include "AnnotationController.h"
#include "gui/Editor.h"
#include "gui/MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QUuid>

namespace AgentAnnotations {
Bridge *Bridge::instance()
{
  static QPointer<Bridge> service;
  if (!service) service = new Bridge(qApp);
  return service;
}
Bridge::Bridge(QObject *parent) : QObject(parent), transport(new Transport)
{
  transport->moveToThread(&thread);
  connect(&thread, &QThread::finished, transport, &QObject::deleteLater);
  connect(transport, &Transport::listening, this, [this](QJsonObject value, QString error) {
    info = value;
    starting = false;
    emit accessChanged(info, error);
    if (!info.isEmpty()) {
      heartbeat.start(1000);
      publish();
    } else heartbeat.stop();
  });
  connect(transport, &Transport::operation, this, &Bridge::dispatch);
  connect(&heartbeat, &QTimer::timeout, this, &Bridge::publish);
  thread.start();
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { stop(false); });
}
Bridge::~Bridge()
{
  QMetaObject::invokeMethod(transport, [this] { transport->stop(); }, Qt::BlockingQueuedConnection);
  thread.quit();
  thread.wait();
}
void Bridge::start(bool remember)
{
  if (remember) QSettings().setValue("annotations/agentAccess", true);
  if (starting || !info.isEmpty()) return;
  starting = true;
  QMetaObject::invokeMethod(transport, [this] { transport->start(); }, Qt::QueuedConnection);
}
void Bridge::stop(bool remember)
{
  if (remember) {
    QSettings().setValue("annotations/agentAccess", false);
    QSettings().setValue("agentServer/enabled", false);
  }
  info = {};
  starting = false;
  heartbeat.stop();
  sources.clear();
  trees.clear();
  parseOutcomes.clear();
  QMetaObject::invokeMethod(transport, [this] { transport->stop(); }, Qt::BlockingQueuedConnection);
  emit accessChanged({}, {});
}
void Bridge::startAgent(bool remember)
{
  if (remember) QSettings().setValue("agentServer/enabled", true);
  if (info.value("access_mode") == "agent") return;
  // Upgrading consent starts a new server identity, invalidating old queued work.
  stop(false);
  starting = true;
  QMetaObject::invokeMethod(transport, [this] { transport->start(true); }, Qt::QueuedConnection);
}
void Bridge::showAccess(QWidget *parent)
{
  auto dialog = new QDialog(parent);
  dialog->setObjectName("agentAccessDialog");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(tr("Agent Access"));
  auto layout = new QVBoxLayout(dialog);
  auto explanation = new QLabel(tr("Allow local agents to inspect open documents, model structure and annotations, "
                                   "and control and capture viewports. Access is remembered across launches."), dialog);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto state = new QLabel(dialog);
  state->setTextFormat(Qt::PlainText);
  state->setWordWrap(true);
  layout->addWidget(state);
  auto enable = new QPushButton(tr("Enable agent access"), dialog);
  enable->setObjectName("enableAgentAccess");
  auto stopButton = new QPushButton(tr("Stop agent access"), dialog);
  stopButton->setObjectName("stopAgentAccess");
  auto copy = new QPushButton(tr("Copy connection info"), dialog);
  copy->setObjectName("copyAgentConnection");
  layout->addWidget(enable);
  layout->addWidget(stopButton);
  layout->addWidget(copy);
  connect(enable, &QPushButton::clicked, this, [this] { startAgent(); });
  connect(stopButton, &QPushButton::clicked, this, [this] { stop(); });
  connect(copy, &QPushButton::clicked, this, [this] {
    if (!info.isEmpty()) QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(info).toJson()));
  });
  auto update = [state, enable, stopButton, copy](QJsonObject value, QString error) {
    const bool live = !value.isEmpty();
    enable->setEnabled(value.value("access_mode") != "agent");
    stopButton->setEnabled(live);
    copy->setEnabled(live);
    state->setText(!error.isEmpty() ? error : !live ? tr("Agent access is stopped.") :
      value.value("url").toString() + "\n" + (value.value("access_mode") == "agent" ? tr("Agent access enabled") : tr("Annotations-only access")));
  };
  connect(this, &Bridge::accessChanged, dialog, update);
  update(info, {});
  dialog->resize(440, 240);
  dialog->show();
}
void Bridge::registerController(Controller *controller)
{
  controllers.append(controller);
  connect(controller, &Controller::changed, this, &Bridge::publish);
  connect(controller, &QObject::destroyed, this,
          [this] { QTimer::singleShot(0, this, &Bridge::publish); });
  auto widget = new QWidget;
  auto layout = new QVBoxLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0);
  auto enable = new QCheckBox(tr("Enable local agent access"), widget);
  auto label = new QLabel(widget);
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  auto copy = new QPushButton(tr("Copy connection info"), widget);
  auto stop = new QPushButton(tr("Stop agent access"), widget);
  layout->addWidget(enable);
  layout->addWidget(label);
  layout->addWidget(copy);
  layout->addWidget(stop);
  connect(enable, &QCheckBox::toggled, this, [this](bool enabled) {
    if (enabled) start();
    else this->stop();
  });
  connect(stop, &QPushButton::clicked, this, [this] { this->stop(); });
  connect(copy, &QPushButton::clicked, this, [this] {
    if (!info.isEmpty())
      QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(info).toJson()));
  });
  auto update = [enable, label, copy, stop](QJsonObject info, QString error) {
    QSignalBlocker blocker(enable);
    enable->setChecked(!info.isEmpty());
    copy->setEnabled(!info.isEmpty());
    stop->setEnabled(!info.isEmpty());
    label->setText(!error.isEmpty() ? error
                   : info.isEmpty() ? tr("Agent access is stopped.")
                                    : info.value("url").toString());
  };
  connect(this, &Bridge::accessChanged, widget, update);
  update(info, {});
  controller->setAccessWidget(widget);
  if (QSettings().value("agentServer/enabled", false).toBool()) startAgent(false);
  else if (QSettings().value("annotations/agentAccess", false).toBool()) start(false);
  publish();
}
void Bridge::publish()
{
  if (info.isEmpty()) return;
  Snapshot snapshot;
  QJsonArray documents, viewports;
  QSet<QString> liveDocuments;
  for (auto it = controllers.begin(); it != controllers.end();) {
    if (!*it) {
      it = controllers.erase(it);
      continue;
    }
    auto controller = it->data();
    ++it;
    controller->pruneInspection();
    viewports.append(controller->status());
    for (auto editor : controller->editors()) {
      const auto id = editor->annotations->sessionId;
      liveDocuments.insert(id);
      documents.append(
        QJsonObject{{"document_id", id},
                    {"window_id", controller->windowId},
                    {"path", editor->filepath},
                    {"source_dirty", editor->isContentModified()},
                    {"annotation_dirty", editor->annotations->dirty()},
                    {"customizer_dirty", editor->parameterWidget->isModified()},
                    {"source_sha256", editor->sourceFingerprint},
                    {"source_revision", QString::number(editor->sourceRevision)},
                    {"source_hash_origin", "editor_utf8"},
                    {"annotation_revision", QString::number(editor->annotations->revision())},
                    {"diagnostic", editor->annotations->diagnostic()}});
      snapshot.records.insert(id, editor->annotations->records());
      const auto inspection = controller->inspection(id);
      for (const auto& kind : {"attempts", "results", "diagnostics"})
        snapshot.reads.insert("/v2/documents/" + id + "/" + kind,
          {{"document_id", id}, {kind, inspection.value(kind)}, {"history_complete", false}});
      snapshot.reads.insert("/v2/documents/" + id + "/dependencies",
        {{"document_id", id}, {"dependencies", QJsonArray{}}, {"complete", false},
         {"reason", "Dependency bytes have not been captured for this document."}});
    }
  }
  for (auto it = sources.begin(); it != sources.end();) {
    if (it->expires.hasExpired() || !liveDocuments.contains(it->metadata.value("document_id").toString())) {
      it = sources.erase(it);
      continue;
    }
    for (auto page = it->pages.cbegin(); page != it->pages.cend(); ++page) {
      snapshot.reads.insert(page.key(), page.value());
      snapshot.readExpiry.insert(page.key(), it->expires);
    }
    ++it;
  }
  snapshot.status = {{"documents", documents}, {"viewports", viewports}};
  for (auto it = trees.begin(); it != trees.end();) {
    if (it->expires.hasExpired() || !liveDocuments.contains(it->document)) { it = trees.erase(it); continue; }
    for (auto page = it->pages.cbegin(); page != it->pages.cend(); ++page) {
      snapshot.reads.insert(page.key(), page.value());
      snapshot.readExpiry.insert(page.key(), it->expires);
    }
    ++it;
  }
  for (auto it = parseOutcomes.begin(); it != parseOutcomes.end();) {
    if (!liveDocuments.contains(it.key())) { it = parseOutcomes.erase(it); continue; }
    const auto base = "/v2/documents/" + it.key();
    snapshot.reads.insert(base + "/parse-result", it.value());
    auto diagnostics = snapshot.reads.value(base + "/diagnostics");
    diagnostics["latest_parse"] = it->value("diagnostics");
    diagnostics["latest_parse_id"] = it->value("parse_id");
    snapshot.reads.insert(base + "/diagnostics", diagnostics);
    snapshot.reads.insert(base + "/dependencies", {{"document_id", it.key()},
      {"scope", "parse"}, {"snapshot_id", it->value("snapshot_id")},
      {"dependencies", it->value("dependencies")}, {"complete", it->value("dependencies_complete")}});
    ++it;
  }
  QMetaObject::invokeMethod(
    transport,
    [transport = transport, snapshot = std::move(snapshot)]() mutable {
      transport->publish(std::move(snapshot));
    },
    Qt::QueuedConnection);
}
void Bridge::dispatch(QString server, QString id, QString path, QJsonObject body)
{
  bool live = false;
  QMetaObject::invokeMethod(
    transport, [this, &live, server, id] { live = transport->beginJob(server, id); },
    Qt::BlockingQueuedConnection);
  if (!live) return;
  auto done = [this, server, id](QJsonObject result, QByteArray png, QString error) {
    QMetaObject::invokeMethod(
      transport,
      [transport = transport, server, id, result, png, error] {
        transport->complete(server, id, result, png, error);
      },
      Qt::QueuedConnection);
  };
  if (path.startsWith("/v2/documents/")) {
    for (const auto& controller : controllers) {
      if (!controller || controller->windowId != body.value("window_id")) continue;
      for (auto editor : controller->editors()) {
        if (editor->annotations->sessionId != body.value("document_id")) continue;
        QString error;
        QJsonObject result;
        if (path.endsWith("/parse")) {
          const auto source = sources.constFind(body.value("snapshot_id").toString());
          if (source == sources.cend() || source->expires.hasExpired() || source->metadata.value("document_id") != body.value("document_id"))
            error = "Source snapshot expired or belongs to another document.";
          else if (controller->busy() || QApplication::activeModalWidget()) error = "Native compilation or interaction is busy.";
          else {
            const GuiLocker lock;
            auto parsed = inspectSyntax(source->metadata, source->text);
            result = parsed.outcome;
            result["editor_advanced"] = source->metadata.value("source_revision").toString() != QString::number(editor->sourceRevision);
            if (!parsed.tree.metadata.isEmpty() && !retainTree(parsed.tree)) {
              result.remove("tree_id"); result["success"] = false; result["error"] = "Tree retention budget exceeded.";
            }
            parseOutcomes.insert(editor->annotations->sessionId, result);
          }
        } else result = snapshotSource(editor, body, &error);
        publish();
        done(result, {}, error);
        return;
      }
    }
    done({}, {}, "Target document/window is no longer available.");
    return;
  }
  Controller *target = nullptr;
  for (const auto& controller : controllers)
    if (controller && controller->viewportId == body.value("viewport_id").toString() &&
        controller->windowId == body.value("window_id").toString()) {
      target = controller;
      break;
    }
  if (!target || !target->owner() ||
      target->owner()->annotations->sessionId != body.value("document_id")) {
    done({}, {}, "Document/viewport session is no longer available.");
    return;
  }
  if (target->busy()) {
    done({}, {}, "Viewport is busy.");
    return;
  }
  const auto expected = body.value("expected_result_id").toString();
  if (target->status().value("result").toObject().value("id").toString() != expected) {
    done({}, {}, "Rendered result changed.");
    return;
  }
  if (path == "/v1/capture") {
    target->capture(expected, done);
    return;
  }
  QJsonObject camera;
  if (path.startsWith("/v1/annotations/")) {
    auto annotation = path.section('/', 3, 3);
    auto record = target->owner()->annotations->find(annotation);
    if (!record) {
      done({}, {}, "Annotation no longer exists.");
      return;
    }
    camera = record->metadata.value("camera").toObject();
  } else {
    if (path.section('/', 3, 3) != target->viewportId) {
      done({}, {}, "Viewport route does not match target.");
      return;
    }
    camera = body.value("camera").toObject();
  }
  QString error;
  if (!target->setCamera(camera, &error)) done({}, {}, error);
  else done(target->status(), {}, {});
}
QJsonObject Bridge::snapshotSource(EditorInterface *editor, const QJsonObject& request, QString *error)
{
  constexpr qint64 maxBytes = 4 * 1024 * 1024;
  const auto origin = request.value("origin").toString();
  QString sourcePath = editor->filepath;
  if (request.contains("dependency_id")) {
    sourcePath.clear();
    for (const auto& entry : parseOutcomes.value(editor->annotations->sessionId).value("dependencies").toArray()) {
      const auto dependency = entry.toObject();
      if (dependency.value("dependency_id") == request.value("dependency_id") && dependency.value("available").toBool())
        sourcePath = dependency.value("resolved_path").toString();
    }
    if (sourcePath.isEmpty() || origin != "disk") {
      *error = "Dependency handle unavailable; dependencies are read explicitly from disk.";
      return {};
    }
  }
  QByteArray bytes;
  if (origin == "editor") {
    if (request.value("expected_source_revision").toString() != QString::number(editor->sourceRevision)) {
      *error = "Stale or missing editor revision.";
      return {};
    }
    const auto text = editor->toPlainText();
    if (text.size() > maxBytes || (bytes = text.toUtf8()).size() > maxBytes) {
      *error = "Source exceeds 4 MiB.";
      return {};
    }
  } else if (origin == "disk") {
    if (sourcePath.isEmpty()) {
      *error = "Untitled document has no disk source.";
      return {};
    }
    QFile file(sourcePath);
    QFileInfo before(file);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxBytes) {
      *error = "Disk source unavailable or exceeds 4 MiB.";
      return {};
    }
    bytes = file.read(maxBytes + 1);
    QFileInfo after(sourcePath);
    if (bytes.size() > maxBytes || file.error() != QFile::NoError || before.size() != after.size() ||
        before.lastModified() != after.lastModified() || before.canonicalFilePath() != after.canonicalFilePath()) {
      *error = "Disk source changed during read or exceeded its budget.";
      return {};
    }
  } else {
    *error = "Explicit disk or editor origin is required.";
    return {};
  }
  QString text = QString::fromUtf8(bytes);
  auto encoded = text.toUtf8();
  auto original = bytes;
  if (original.startsWith("\xef\xbb\xbf")) original.remove(0, 3);
  if (encoded.startsWith("\xef\xbb\xbf")) encoded.remove(0, 3);
  if (encoded != original) {
    *error = "Source is not valid UTF-8.";
    return {};
  }
  if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
  text.replace("\r\n", "\n");
  text.replace('\r', '\n');
  const auto hash = QString::fromLatin1(Store::hash(text.toUtf8()));
  if (request.contains("expected_source_sha256") && request.value("expected_source_sha256") != hash) {
    *error = "Source hash changed.";
    return {};
  }
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonObject metadata{{"snapshot_id", id}, {"document_id", editor->annotations->sessionId},
    {"origin", origin}, {"path", sourcePath}, {"source_revision", QString::number(editor->sourceRevision)},
    {"source_sha256", hash}, {"disk_sha256", origin == "disk" ? QJsonValue(QString::fromLatin1(Store::hash(bytes))) : QJsonValue(QJsonValue::Null)},
    {"normalization", "utf8_lf"}, {"byte_count", bytes.size()}, {"filesystem_atomic", false},
    {"source_dirty", editor->isContentModified()}, {"editor_utf8_sha256", editor->sourceFingerprint},
    {"captured_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
  auto retainedBytes = [&] {
    qint64 bytes = text.size() * 4 + 65536;
    for (const auto& source : sources) bytes += source.text.size() * 4 + 65536;
    return bytes;
  };
  while (!sources.isEmpty() && (sources.size() >= 8 || retainedBytes() > 32 * 1024 * 1024)) {
    auto oldest = sources.begin();
    for (auto it = sources.begin(); it != sources.end(); ++it)
      if (it->expires.remainingTime() < oldest->expires.remainingTime()) oldest = it;
    sources.erase(oldest);
  }
  Source source{metadata, text, QDeadlineTimer(300000), {}};
  const QString route = "/v2/source-snapshots/" + id;
  int offset = 0;
  for (int page = 0; offset < text.size() || page == 0; ++page) {
    int end = qMin(offset + 16384, int(text.size()));
    if (end < text.size() && text.at(end - 1).isHighSurrogate()) --end;
    auto value = metadata;
    value["text"] = text.mid(offset, end - offset);
    value["page"] = page;
    value["next_cursor"] = end < text.size() ? QJsonValue(route + "/pages/" + QString::number(page + 1)) : QJsonValue(QJsonValue::Null);
    source.pages.insert(route + "/pages/" + QString::number(page), value);
    if (page == 0) source.pages.insert(route, value);
    offset = end;
  }
  sources.insert(id, std::move(source));
  return metadata;
}
bool Bridge::retainTree(const InspectionTree& tree)
{
  auto pages = treePages(tree);
  qint64 bytes = 0;
  for (const auto& page : pages) {
    const auto size = QJsonDocument(page).toJson(QJsonDocument::Compact).size();
    if (size > 1024 * 1024) return false;
    bytes += size * 2 + 256; // Account for the detached Qt representation and per-route overhead.
  }
  if (bytes > 32 * 1024 * 1024) return false;
  auto total = [&] { qint64 size = bytes; for (const auto& entry : trees) size += entry.bytes; return size; };
  while (!trees.isEmpty() && (trees.size() >= 8 || total() > 32 * 1024 * 1024)) {
    auto oldest = trees.begin();
    for (auto it = trees.begin(); it != trees.end(); ++it)
      if (it->expires.remainingTime() < oldest->expires.remainingTime()) oldest = it;
    trees.erase(oldest);
  }
  trees.insert(tree.metadata.value("tree_id").toString(),
    {tree.metadata.value("document_id").toString(), std::move(pages), QDeadlineTimer(300000), bytes});
  return true;
}
}  // namespace AgentAnnotations
