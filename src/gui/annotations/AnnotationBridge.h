#pragma once
#include "AnnotationTransport.h"
#include <QPointer>
#include <QThread>
#include <QDeadlineTimer>
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
  void startAgent(bool remember = true);
  void showAccess(QWidget *parent);
  void stop(bool remember = true);
  QJsonObject connection() const { return info; }
  void publish();
  QJsonObject snapshotSource(EditorInterface *editor, const QJsonObject& request, QString *error);
signals:
  void accessChanged(QJsonObject info, QString error);

private:
  QThread thread;
  Transport *transport;
  QVector<QPointer<Controller>> controllers;
  QJsonObject info;
  QTimer heartbeat;
  bool starting = false;
  struct Source {
    QJsonObject metadata;
    QString text;
    QDeadlineTimer expires;
    QHash<QString, QJsonObject> pages;
  };
  QMap<QString, Source> sources;
  void dispatch(QString server, QString id, QString path, QJsonObject body);
};
}  // namespace AgentAnnotations
