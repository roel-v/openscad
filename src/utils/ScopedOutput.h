#pragma once
#include <memory>
struct Message;
// Isolated parser output only. Caller serializes with native compilation.
class ScopedOutput {
public:
  ScopedOutput(void (*handler)(const Message&, void *), void *data);
  ~ScopedOutput();
  ScopedOutput(const ScopedOutput&) = delete;
  ScopedOutput& operator=(const ScopedOutput&) = delete;
private:
  struct State;
  std::unique_ptr<State> state;
};
