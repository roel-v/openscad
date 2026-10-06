#include "glview/SelectionRouter.h"
#include <cmath>
#include <utility>
SelectionRouter::Token SelectionRouter::begin(Mode mode,std::function<void(CancelReason)> cancelled)
{
  cancel(active,CancelReason::Replaced);
  // A cancellation callback may intentionally arm a replacement of its own.
  if (active) return 0;
  active=++next; activeMode=mode; cancellation=std::move(cancelled); pressed=moved=false;
  return active;
}
void SelectionRouter::cancel(Token token,CancelReason reason)
{
  if (!token || token!=active) return;
  auto callback=std::move(cancellation); finish(token);
  if (callback) callback(reason);
}
void SelectionRouter::finish(Token token)
{
  if (!token || token!=active) return;
  active=0; cancellation={}; pressed=moved=false;
}
bool SelectionRouter::accept(Token token,Outcome outcome)
{
  if (!token || token!=active) return false;
  if (outcome!=Outcome::Miss) finish(token);
  return true;
}
void SelectionRouter::press(double x,double y) { pressX=x; pressY=y; pressed=true; moved=false; }
void SelectionRouter::move(double x,double y,double threshold)
{
  if (pressed && std::abs(x-pressX)+std::abs(y-pressY)>=threshold) moved=true;
}
