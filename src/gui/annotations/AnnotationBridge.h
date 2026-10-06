#pragma once
#include "AnnotationTransport.h"
#include "AgentSyntax.h"
#include <QPointer>
#include <QThread>
#include <QDeadlineTimer>
#include <QJsonArray>
class QWidget;
class EditorInterface;
namespace AgentAnnotations {
class Controller;
class Bridge : public QObject
{
  Q_OBJECT
public:
  static Bridge *instance();
  explicit Bridge(QObject *parent);
  ~Bridge() override;
  void registerController(Controller *controller);
  void start(bool remember = true);
  void showAccess(QWidget *parent);
  void stop(bool remember = true);
  QJsonObject connection() const { return info; }
  void publish();
  QJsonObject snapshotSource(EditorInterface *editor, const QJsonObject& request, QString *error);
  bool retainTree(const InspectionTree& tree);
signals:
  void accessChanged(QJsonObject info, QString error);

private:
  QThread thread;
  Transport *transport;
  QVector<QPointer<Controller>> controllers;
  QJsonObject info;
  QTimer heartbeat;
  QJsonArray lastCameras;
  void publishCameras();
  bool starting = false;
  struct Source {
    QJsonObject metadata;
    QString text;
    QDeadlineTimer expires;
    QHash<QString, QJsonObject> pages;
  };
  QMap<QString, Source> sources;
  struct TreeEntry {
    QString document;
    QHash<QString, QJsonObject> pages;
    QDeadlineTimer expires;
    qint64 bytes;
  };
  QMap<QString, TreeEntry> trees;
  QHash<QString, QJsonObject> parseOutcomes;
  void dispatch(QString server, QString id, QString path, QJsonObject body);
};
}  // namespace AgentAnnotations
