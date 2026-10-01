#include "AnnotationBridge.h"
#include "AnnotationController.h"
#include "gui/Editor.h"
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
  for (auto it = controllers.begin(); it != controllers.end();) {
    if (!*it) {
      it = controllers.erase(it);
      continue;
    }
    auto controller = it->data();
    ++it;
    viewports.append(controller->status());
    for (auto editor : controller->editors()) {
      const auto id = editor->annotations->sessionId;
      documents.append(
        QJsonObject{{"document_id", id},
                    {"window_id", controller->windowId},
                    {"path", editor->filepath},
                    {"source_dirty", editor->isContentModified()},
                    {"annotation_dirty", editor->annotations->dirty()},
                    {"customizer_dirty", editor->parameterWidget->isModified()},
                    {"source_sha256", editor->sourceFingerprint},
                    {"annotation_revision", QString::number(editor->annotations->revision())},
                    {"diagnostic", editor->annotations->diagnostic()}});
      snapshot.records.insert(id, editor->annotations->records());
    }
  }
  snapshot.status = {{"documents", documents}, {"viewports", viewports}};
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
}  // namespace AgentAnnotations
