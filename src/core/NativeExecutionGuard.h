#pragma once
#include <mutex>
#include "utils/StackCheck.h"
// Parser state, native evaluation state and geometry caches share one gate.
// Recursive entry is required for library parsing and nested GeometryEvaluator calls.
class NativeExecutionGuard {
public:
  NativeExecutionGuard() : lock(gate()) { (void)StackCheck::inst(); }
  NativeExecutionGuard(const NativeExecutionGuard&) = delete;
  NativeExecutionGuard& operator=(const NativeExecutionGuard&) = delete;
private:
  static std::recursive_mutex& gate() { static std::recursive_mutex value; return value; }
  std::unique_lock<std::recursive_mutex> lock;
};
