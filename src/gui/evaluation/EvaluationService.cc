#include "gui/evaluation/EvaluationService.h"
#include "core/NativeExecutionGuard.h"
#include <QApplication>
#include <QMetaObject>
#include <QProgressDialog>
#include <algorithm>
#include <exception>
#ifdef ENABLE_PYTHON
#include "python/python_public.h"
#endif

namespace BackgroundEvaluation {
Service& Service::instance()
{
  static Service *service = new Service;
  return *service;
}
Service::Service() : QObject(qApp), worker(new QObject)
{
  qApp->setQuitOnLastWindowClosed(false);
  thread.setObjectName("SCAD evaluation");
  thread.setStackSize(8 * 1024 * 1024);
  worker->moveToThread(&thread);
  connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
  thread.start();
}
Service::~Service()
{
  // Normal application exit is delayed by shutdownWhenIdle until every native
  // job has unwound. No per-window destructor owns or joins this worker.
  if (active) active->cancellation->store(true);
  thread.quit();
  thread.wait();
#ifdef ENABLE_PYTHON
  // Static Python-owned values are destroyed on the application thread.
  python_lock();
#endif
}
void Service::retire(std::function<void()> release)
{
  // Only CPU ownership enters this path, never widgets or GL resources.
  QMetaObject::invokeMethod(worker, [release = std::move(release)] {
    NativeExecutionGuard execution;
    release();
  }, Qt::QueuedConnection);
}
bool Service::submit(Job::Ptr job)
{
  Q_ASSERT(QThread::currentThread() == QObject::thread());
  if (closing) return false;
  job->queued = Job::Clock::now();
  if (job->automatic) {
    if (active && active->key == job->key && !active->automatic && !active->cancellation->load()) return false;
    for (const auto& queued : pending)
      if (queued->key == job->key && !queued->automatic && !queued->cancellation->load()) return false;
  }
  if (active && active->key == job->key) active->cancellation->store(true);
  for (auto& queued : pending) {
    if (queued->key != job->key) continue;
    queued->cancellation->store(true);
    auto replaced = std::move(queued);
    replaced->finished = Job::Clock::now();
    queued = std::move(job); // Preserve this document's place in the queue.
    auto complete = std::move(replaced->complete);
    replaced->compute = {};
    if (complete) complete();
    return true;
  }
  pending.push_back(std::move(job));
  dispatch();
  return true;
}
void Service::cancel(const QString& key)
{
  if (active && active->key == key) active->cancellation->store(true);
  std::deque<Job::Ptr> removed;
  for (auto it = pending.begin(); it != pending.end();) {
    if ((*it)->key != key) { ++it; continue; }
    (*it)->cancellation->store(true);
    removed.push_back(*it);
    it = pending.erase(it);
  }
  for (auto& job : removed) {
    job->finished = Job::Clock::now();
    auto complete = std::move(job->complete);
    job->compute = {};
    if (complete) complete();
  }
}
void Service::shutdownWhenIdle()
{
  closing = true;
  if (active) active->cancellation->store(true);
  auto removed = std::move(pending);
  pending.clear();
  for (auto& job : removed) {
    job->finished = Job::Clock::now();
    job->cancellation->store(true);
    auto complete = std::move(job->complete);
    job->compute = {};
    if (complete) complete();
  }
  if (active && !shutdownDialog) {
    shutdownDialog = new QProgressDialog(tr("Cancelling evaluation before closing…"), QString(), 0, 0);
    shutdownDialog->setWindowTitle(tr("Closing OpenSCAD"));
    shutdownDialog->setCancelButton(nullptr);
    shutdownDialog->setAttribute(Qt::WA_DeleteOnClose);
    shutdownDialog->show();
  }
  if (!active) finishShutdown();
}
void Service::finishShutdown()
{
  if (shutdownQueued) return;
  shutdownQueued = true;
  // Drain already queued CPU retirement before ending the GUI event loop.
  QMetaObject::invokeMethod(worker, [this] {
    QMetaObject::invokeMethod(this, [this] {
      if (shutdownDialog) shutdownDialog->close();
      qApp->quit();
    }, Qt::QueuedConnection);
  }, Qt::QueuedConnection);
}
void Service::dispatch()
{
  if (active || pending.empty()) return;
  auto next = std::find_if(pending.begin(), pending.end(), [](const auto& job) { return !job->automatic; });
  if (next == pending.end()) next = pending.begin();
  active = *next;
  pending.erase(next);
  const auto job = active;
  QMetaObject::invokeMethod(worker, [this, job] {
    job->started = Job::Clock::now();
    EvaluationCancellationScope cancellation(job->cancellation);
    try {
      job->setStage("Waiting for native evaluation");
      NativeExecutionGuard execution;
      evaluationCheckpoint();
      job->compute();
      evaluationCheckpoint();
      job->succeeded = true;
    } catch (const EvaluationCancelled&) {
      job->setStage("Cancelled");
    } catch (const std::exception& error) {
      job->error = QString::fromUtf8(error.what());
    } catch (...) {
      job->error = "Unknown evaluation exception";
    }
    job->finished = Job::Clock::now();
    QMetaObject::invokeMethod(this, [this, job] {
      active.reset();
      if (job->complete) job->complete();
      job->compute = {}; job->complete = {};
      if (closing) { if (!active) finishShutdown(); }
      else dispatch();
    }, Qt::QueuedConnection);
  }, Qt::QueuedConnection);
}
}
