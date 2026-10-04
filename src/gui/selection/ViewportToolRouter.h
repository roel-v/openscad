#pragma once
#include "glview/SelectionRouter.h"
#include "gui/selection/SelectionTypes.h"
#include <QObject>
#include <QPointer>
#include <QPointF>
class MainWindow;
class QEvent;
namespace Selection {
using CancelReason=SelectionRouter::CancelReason;
using ToolToken=SelectionRouter::Token;
struct ToolRequest { QString owner; Stamp expected; bool needNormal=true; };
struct DragCallbacks {
  std::function<void(QPointF,Qt::KeyboardModifiers)> move,release;
  std::function<void(CancelReason)> cancel;
};
class ViewportToolRouter : public QObject {
  Q_OBJECT
public:
  explicit ViewportToolRouter(MainWindow *window);
  ~ViewportToolRouter() override;
  ToolToken beginPick(ToolRequest request,QObject *lifetime,std::function<void(PickResult)> callback);
  ToolToken beginDrag(ToolRequest request,QObject *lifetime,QPointF press,DragCallbacks callbacks,Qt::MouseButton button=Qt::LeftButton);
  // Legacy tools retain their existing input handler, but share exclusive ownership.
  ToolToken claimExternal(QString owner,QObject *lifetime,std::function<void(CancelReason)> cancelled);
  void cancel(ToolToken token,CancelReason reason);
  void finish(ToolToken token);
  ToolToken active() const { return router.token(); }
  bool route(QEvent& event);
signals:
  void changed();
  void feedback(const QString& message);
private:
  QPointer<MainWindow> window;
  SelectionRouter router;
  ToolRequest request;
  QPointer<QObject> owner;
  std::function<void(PickResult)> pick;
  DragCallbacks drag;
  QMetaObject::Connection lifetime;
  Qt::MouseButton dragButton=Qt::LeftButton;
  bool capturedDrag=false;
  ToolToken arm(SelectionRouter::Mode mode,QObject *owner,std::function<void(CancelReason)> cancelled);
  void retire();
  bool eventFilter(QObject *object,QEvent *event) override;
};
}
