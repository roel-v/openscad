#include "gui/selection/ViewportToolRouter.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/MainWindow.h"
#include "gui/TabManager.h"
#include "gui/QGLView.h"
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>

namespace Selection {
ViewportToolRouter::ViewportToolRouter(MainWindow *window) : QObject(window->qglview),window(window)
{
  window->qglview->installEventFilter(this);
  connect(window->evaluationBinding,&EvaluationBinding::invalidated,this,[this] {
    if (router.mode()!=SelectionRouter::Mode::External && active()) cancel(active(),CancelReason::ResultChanged);
  });
  connect(window->tabManager,&TabManager::currentEditorChanged,this,[this](auto) { cancel(active(),CancelReason::DocumentChanged); });
}
ViewportToolRouter::~ViewportToolRouter() { cancel(active(),CancelReason::OwnerDestroyed); }
void ViewportToolRouter::retire()
{
  QObject::disconnect(lifetime); owner.clear(); pick={}; drag={};
  if (capturedDrag) { capturedDrag=false; window->qglview->releaseMouse(); }
  window->qglview->unsetCursor(); emit changed();
}
ToolToken ViewportToolRouter::arm(SelectionRouter::Mode mode,QObject *lifetimeOwner,std::function<void(CancelReason)> cancelled)
{
  if (!lifetimeOwner) return 0;
  const auto token=router.begin(mode,[this,cancelled=std::move(cancelled)](auto reason) {
    retire(); if (cancelled) cancelled(reason);
  });
  if (!token) return 0;
  owner=lifetimeOwner;
  lifetime=connect(lifetimeOwner,&QObject::destroyed,this,[this,token] { cancel(token,CancelReason::OwnerDestroyed); });
  emit changed(); return token;
}
ToolToken ViewportToolRouter::beginPick(ToolRequest next,QObject *lifetimeOwner,std::function<void(PickResult)> callback)
{
  if (!lifetimeOwner || next.owner.isEmpty() || !callback || !window->evaluationBinding->validate(next.expected)) {
    if (callback) callback({PickError::Stale,{},tr("Pick request belongs to a stale or inactive result")}); return 0;
  }
  const auto token=arm(SelectionRouter::Mode::Pick,lifetimeOwner,[callback](auto) { callback({PickError::Cancelled}); });
  if (!token) return 0;
  request=std::move(next); pick=std::move(callback);
  window->qglview->setFocus(); window->qglview->setCursor(Qt::CrossCursor); return token;
}
ToolToken ViewportToolRouter::beginDrag(ToolRequest next,QObject *lifetimeOwner,QPointF press,DragCallbacks callbacks,Qt::MouseButton button)
{
  if (!lifetimeOwner || next.owner.isEmpty() || !window->evaluationBinding->validate(next.expected)) return 0;
  const auto token=arm(SelectionRouter::Mode::Drag,lifetimeOwner,callbacks.cancel);
  if (!token) return 0;
  request=std::move(next); drag=std::move(callbacks); router.press(press.x(),press.y());
  dragButton=button; capturedDrag=true; window->qglview->grabMouse();
  window->qglview->setFocus(); return token;
}
ToolToken ViewportToolRouter::claimExternal(QString owner,QObject *lifetimeOwner,std::function<void(CancelReason)> cancelled)
{
  if (owner.isEmpty()) return 0;
  return arm(SelectionRouter::Mode::External,lifetimeOwner,std::move(cancelled));
}
void ViewportToolRouter::cancel(ToolToken token,CancelReason reason) { router.cancel(token,reason); }
void ViewportToolRouter::finish(ToolToken token)
{
  if (!token || active()!=token) return;
  router.finish(token); retire();
}
bool ViewportToolRouter::route(QEvent& event)
{
  if (!active() || QApplication::activeModalWidget()) return false;
  if (event.type()==QEvent::ShortcutOverride || event.type()==QEvent::KeyPress) {
    auto& key=static_cast<QKeyEvent&>(event);
    if (key.key()==Qt::Key_Escape) {
      if (event.type()==QEvent::KeyPress) cancel(active(),CancelReason::Escape);
      event.accept(); return true;
    }
    return false;
  }
  if (router.mode()==SelectionRouter::Mode::External) return false;
  if (!window->evaluationBinding->validate(request.expected)) { cancel(active(),CancelReason::ResultChanged); return false; }
  if (event.type()!=QEvent::MouseButtonPress && event.type()!=QEvent::MouseButtonRelease && event.type()!=QEvent::MouseMove) return false;
  auto& mouse=static_cast<QMouseEvent&>(event); const auto point=mouse.position();
  if (router.mode()==SelectionRouter::Mode::Drag) {
    if (event.type()==QEvent::MouseMove) { if (drag.move) drag.move(point,mouse.modifiers()); return true; }
    if (event.type()==QEvent::MouseButtonRelease && mouse.button()==dragButton) {
      auto release=drag.release; finish(active()); if (release) release(point,mouse.modifiers()); return true;
    }
    return mouse.button()==dragButton;
  }
  if (event.type()==QEvent::MouseButtonPress && mouse.button()==Qt::LeftButton && mouse.modifiers()==Qt::NoModifier)
    router.press(point.x(),point.y());
  else if (event.type()==QEvent::MouseMove) router.move(point.x(),point.y(),QApplication::startDragDistance());
  else if (event.type()==QEvent::MouseButtonRelease && mouse.button()==Qt::LeftButton) {
    const bool click=router.click(); router.releasePress();
    if (!click) return false;
    const auto token=active(); auto callback=pick;
    auto result=window->selectionService->pick(point,request.expected);
    if (!active() || token!=active()) return true;
    const auto outcome=result.error==PickError::Miss ? SelectionRouter::Outcome::Miss :
      result.error==PickError::None ? SelectionRouter::Outcome::Success : SelectionRouter::Outcome::Failure;
    router.accept(token,outcome);
    if (outcome!=SelectionRouter::Outcome::Miss) { retire(); if (callback) callback(result); }
    else emit feedback(tr("No surface at this point. Pick again or press Escape."));
    return true;
  }
  return false;
}
bool ViewportToolRouter::eventFilter(QObject *,QEvent *event)
{
  if (event->type()==QEvent::FocusOut && active() && router.mode()!=SelectionRouter::Mode::External) {
    QTimer::singleShot(0,this,[this] {
      auto focus=QApplication::focusWidget();
      if (active() && (!focus || (focus!=window->qglview && !window->qglview->isAncestorOf(focus)))) cancel(active(),CancelReason::FocusLost);
    });
  }
  if (event->type()==QEvent::Hide) cancel(active(),CancelReason::FocusLost);
  if (event->type()==QEvent::KeyPress || event->type()==QEvent::ShortcutOverride) return route(*event);
  return false;
}
}
