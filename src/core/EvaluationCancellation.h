#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <utility>

class EvaluationCancelled final : public std::exception {
public:
  const char *what() const noexcept override { return "Evaluation cancelled"; }
};

// GUI cancellation writes the token directly. It must never wait for a slot on
// the evaluation thread, whose event loop is occupied by the computation.
using EvaluationCancellation = std::shared_ptr<std::atomic_bool>;
inline thread_local EvaluationCancellation currentEvaluationCancellation;
inline bool evaluationCancelled() noexcept
{
  return currentEvaluationCancellation && currentEvaluationCancellation->load(std::memory_order_relaxed);
}
inline void evaluationCheckpoint()
{
  if (evaluationCancelled()) throw EvaluationCancelled();
}
class EvaluationCancellationScope {
public:
  explicit EvaluationCancellationScope(EvaluationCancellation token)
    : previous(std::exchange(currentEvaluationCancellation, std::move(token))) {}
  ~EvaluationCancellationScope() { currentEvaluationCancellation = std::move(previous); }
  EvaluationCancellationScope(const EvaluationCancellationScope&) = delete;
  EvaluationCancellationScope& operator=(const EvaluationCancellationScope&) = delete;
private:
  EvaluationCancellation previous;
};
