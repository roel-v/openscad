#pragma once
#include "AnnotationTransport.h"
#include <QPointer>
#include <QThread>
class QWidget;
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
  void stop(bool remember = true);
  QJsonObject connection() const { return info; }
  void publish();
signals:
  void accessChanged(QJsonObject info, QString error);

private:
  QThread thread;
  Transport *transport;
  QVector<QPointer<Controller>> controllers;
  QJsonObject info;
  QTimer heartbeat;
  bool starting = false;
  void dispatch(QString server, QString id, QString path, QJsonObject body);
};
}  // namespace AgentAnnotations
