#pragma once
#include <QObject>
#include <QEasingCurve>
#include <QVariantMap>
#include <QWidget>

// Logical pixels and milliseconds, with persistent normal baselines and session-only dev overrides.
class NavigationSettings : public QObject {
  Q_OBJECT
  Q_PROPERTY(double scale MEMBER scale NOTIFY changed)
  Q_PROPERTY(double cubeRight MEMBER cubeRight NOTIFY changed)
  Q_PROPERTY(double cubeTop MEMBER cubeTop NOTIFY changed)
  Q_PROPERTY(double menuRight MEMBER menuRight NOTIFY changed)
  Q_PROPERTY(double menuTop MEMBER menuTop NOTIFY changed)
  Q_PROPERTY(double chamfer MEMBER chamfer NOTIFY changed)
  Q_PROPERTY(double cubeRadius MEMBER cubeRadius NOTIFY changed)
  Q_PROPERTY(double labelSize MEMBER labelSize NOTIFY changed)
  Q_PROPERTY(double arrowSize MEMBER arrowSize NOTIFY changed)
  Q_PROPERTY(double idleOpacity MEMBER idleOpacity NOTIFY changed)
  Q_PROPERTY(double activeOpacity MEMBER activeOpacity NOTIFY changed)
  Q_PROPERTY(int enterDelay MEMBER enterDelay NOTIFY changed)
  Q_PROPERTY(int leaveDelay MEMBER leaveDelay NOTIFY changed)
  Q_PROPERTY(int fadeIn MEMBER fadeIn NOTIFY changed)
  Q_PROPERTY(int fadeOut MEMBER fadeOut NOTIFY changed)
  Q_PROPERTY(bool visible MEMBER visible NOTIFY changed)
  Q_PROPERTY(bool animate MEMBER animate NOTIFY changed)
  Q_PROPERTY(bool freeOrbit MEMBER freeOrbit NOTIFY changed)
  Q_PROPERTY(int duration MEMBER duration NOTIFY changed)
  Q_PROPERTY(int easing MEMBER easing NOTIFY changed)
  Q_PROPERTY(double rollStep MEMBER rollStep NOTIFY changed)
  Q_PROPERTY(double arrowStep MEMBER arrowStep NOTIFY changed)
  Q_PROPERTY(double dragThreshold MEMBER dragThreshold NOTIFY changed)
  Q_PROPERTY(double orbitSensitivity MEMBER orbitSensitivity NOTIFY changed)
  Q_PROPERTY(bool showInformation MEMBER showInformation NOTIFY changed)
  Q_PROPERTY(bool triangles MEMBER triangles NOTIFY changed)
  Q_PROPERTY(bool fps MEMBER fps NOTIFY changed)
  Q_PROPERTY(bool frameTime MEMBER frameTime NOTIFY changed)
  Q_PROPERTY(int statsRefresh MEMBER statsRefresh NOTIFY changed)
  Q_PROPERTY(int statsWindow MEMBER statsWindow NOTIFY changed)
  Q_PROPERTY(int statsIdle MEMBER statsIdle NOTIFY changed)
public:
  explicit NavigationSettings(QObject *parent = nullptr);
  ~NavigationSettings() override;
  void setTemporary(const QByteArray& name, const QVariant& value);
  void resetOverrides();
  static void setPersistent(const QByteArray& name, const QVariant& value);
  static QVariant persistent(const QByteArray& name, const QVariant& fallback);
  static QWidget *preferenceControls(QWidget *parent);
  QVariantMap values() const;
  QString description() const;
  static QString unit(const QByteArray& name);
  double scale = 1.1;
  double cubeRight = 10, cubeTop = 26, menuRight = 8, menuTop = 5;
  double chamfer = 0.22, cubeRadius = 30, labelSize = 10, arrowSize = 20;
  double idleOpacity = 0.45, activeOpacity = 1;
  int enterDelay = 0, leaveDelay = 180, fadeIn = 120, fadeOut = 250;
  bool visible = true, animate = true, freeOrbit = true;
  int duration = 200;
  int easing = QEasingCurve::InOutCubic;
  double rollStep = 90, arrowStep = 15, dragThreshold = 10, orbitSensitivity = 1;
  bool showInformation=false, triangles=true, fps=true, frameTime=false;
  int statsRefresh=250, statsWindow=30, statsIdle=1000;
signals:
  void changed();
private:
  QVariantMap baselines, overrides;
};
