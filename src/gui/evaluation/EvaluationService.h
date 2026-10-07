#pragma once

#include "core/EvaluationCancellation.h"
#include <QObject>
#include <QThread>
#include <QString>
#include <QPointer>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>

class QProgressDialog;

namespace BackgroundEvaluation {
struct Job {
  using Ptr = std::shared_ptr<Job>;
  using Clock = std::chrono::steady_clock;
  Clock::time_point queued, started, finished;
  QString key;
  bool automatic = false;
  EvaluationCancellation cancellation = std::make_shared<std::atomic_bool>(false);
  std::function<void()> compute;
  std::function<void()> complete; // Invoked only on the GUI thread.
  std::mutex mutex;
  QString stage = "Queued", error;
  bool succeeded = false;
  void setStage(QString value) { std::lock_guard<std::mutex> lock(mutex); stage = std::move(value); }
  QString currentStage() { std::lock_guard<std::mutex> lock(mutex); return stage; }
};

// All queue/lifetime decisions live on the GUI thread. Native computation never
// waits for the GUI and cancellation never waits for the worker event loop.
class Service final : public QObject {
public:
  static Service& instance();
  bool submit(Job::Ptr job);
  void cancel(const QString& key);
  void shutdownWhenIdle();
  void retire(std::function<void()> release);
  bool busy() const { return bool(active) || !pending.empty(); }
  Job::Ptr runningFor(const QString& key) const { return active && active->key == key ? active : nullptr; }
  ~Service() override;
private:
  Service();
  void dispatch();
  void finishShutdown();
  QThread thread;
  QObject *worker;
  std::deque<Job::Ptr> pending;
  Job::Ptr active;
  bool closing = false;
  bool shutdownQueued = false;
  QPointer<QProgressDialog> shutdownDialog;
};
}
