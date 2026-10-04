#pragma once
#include <cstdint>
#include <functional>

class SelectionRouter {
public:
  using Token=uint64_t;
  enum class CancelReason { Escape, Replaced, FocusLost, DocumentChanged, ResultChanged, OwnerDestroyed };
  enum class Mode { Pick, Drag, External };
  enum class Outcome { Success, Miss, Failure };
  Token begin(Mode mode,std::function<void(CancelReason)> cancelled);
  void cancel(Token token,CancelReason reason);
  void finish(Token token);
  bool accept(Token token,Outcome outcome);
  Token token() const { return active; }
  Mode mode() const { return activeMode; }
  void press(double x,double y);
  void move(double x,double y,double threshold);
  bool click() const { return pressed && !moved; }
  void releasePress() { pressed=false; }
private:
  Token next=0,active=0;
  Mode activeMode=Mode::Pick;
  std::function<void(CancelReason)> cancellation;
  bool pressed=false,moved=false;
  double pressX=0,pressY=0;
};
