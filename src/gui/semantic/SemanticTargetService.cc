#include "gui/semantic/SemanticTargetService.h"
#include "gui/MainWindow.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/InspectorDock.h"
#include "core/Context.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QMetaObject>
namespace Semantic {
TargetService::TargetService(MainWindow *window) : QObject(window), window(window) {
  setObjectName("semanticTargetService");
  dispatcher = std::shared_ptr<QObject>(new QObject, [](QObject *object){object->deleteLater();});
  connect(window->evaluationBinding, &Selection::EvaluationBinding::installed, this, &TargetService::refresh);
  connect(window->evaluationBinding, &Selection::EvaluationBinding::invalidated, this, &TargetService::refresh);
}
TargetService::~TargetService() { cancel(); if (currentTargets) removeTargets(currentTargets->resultId()); }
bool TargetService::displayCurrent(const Selection::Stamp& expected) const {
  if (!window) return false;
  auto source = expected; source.viewportGeneration = window->evaluationBinding->stamp().viewportGeneration;
  return window->evaluationBinding->displayCurrent(source);
}
bool TargetService::validate(const Selection::Stamp& expected) const {
  if (!window || !window->evaluationBinding->current()) return false;
  auto source = expected; source.viewportGeneration = window->evaluationBinding->stamp().viewportGeneration;
  return window->evaluationBinding->validate(source);
}
void TargetService::cancel() { for (auto& job : jobs) job->store(true); jobs.clear(); if (currentTargets) currentTargets->cancelPending(); emit changed(); }
void TargetService::retry(TargetKey key) { if (currentTargets && validate(stamp)) { currentTargets->clearTarget(key); resolve(std::move(key)); emit changed(); } }
void TargetService::refresh() {
  if (currentTargets && displayCurrent(stamp)) return;
  cancel(); if (currentTargets) removeTargets(currentTargets->resultId()); currentTargets.reset();
  if (window && window->evaluationBinding->current()) {
    stamp = window->evaluationBinding->stamp(); auto scene = window->evaluationBinding->displayScene(stamp);
    if (scene && scene->semantic && scene->semantic->valid()) {
      QPointer<TargetService> service(this);
      auto delivery=dispatcher;
      currentTargets = std::make_shared<Targets>(stamp.resultId.toStdString(), scene->semantic, [service,delivery](TargetKey key) {
        QMetaObject::invokeMethod(delivery.get(),[service,key=std::move(key)]{if(service)service->request(key);},Qt::QueuedConnection);
      });
      publishTargets(currentTargets);
    }
  }
  emit changed();
}
PreparedTarget TargetService::resolve(TargetKey key) {
  if (!validate(stamp)) { PreparedTarget output; output.state=Design::ResolutionState::Changed; output.diagnostic="Semantic evaluation is stale or unavailable"; return output; }
  return resolveForDisplay(std::move(key));
}
PreparedTarget TargetService::resolveForDisplay(TargetKey key) {
  if (!currentTargets || !displayCurrent(stamp)) { PreparedTarget output; output.state=Design::ResolutionState::Changed; output.diagnostic="Semantic evaluation is stale or unavailable"; return output; }
  return currentTargets->resolve(targetToken(std::move(key)), stamp.resultId.toStdString());
}
void TargetService::request(TargetKey key) {
  if (!window || !currentTargets || !validate(stamp)) return;
  auto expected=stamp; expected.viewportGeneration=window->evaluationBinding->stamp().viewportGeneration;
  auto scene=window->evaluationBinding->scene(expected); if (!scene) return;
  auto stopped=std::make_shared<std::atomic<bool>>(false); jobs.push_back(stopped);
  auto targets=currentTargets; auto delivery=dispatcher; QPointer<TargetService> service(this);
  (void)QtConcurrent::run([scene, key, expected, stopped, targets, delivery, service] {
    auto configure=[scene](Context& context){if(scene->backing && scene->backing->builtin) context.apply_variables(*scene->backing->builtin);};
    auto prepared=std::make_shared<PreparedTarget>(prepareTarget(scene->semantic,key,expected.resultId.toStdString(),scene->documentPath,configure,[stopped]{return stopped->load();}));
    QMetaObject::invokeMethod(delivery.get(), [service, targets, prepared, expected, stopped] {
      if (!service || stopped->load() || service->currentTargets!=targets || !service->validate(expected)) return;
      targets->install(prepared); emit service->changed(); if (service->window) service->window->inspectorDock->refresh();
    }, Qt::QueuedConnection);
  });
}
bool TargetService::exportTarget(TargetKey key, ExportSpace space, QString destination, ExportInfo info) {
  auto target=resolve(std::move(key)); if (target.state!=Design::ResolutionState::Resolved || !validate(stamp)) { emit exportFinished(false,QString::fromStdString(target.diagnostic)); return false; }
  auto stopped=std::make_shared<std::atomic<bool>>(false); jobs.push_back(stopped); auto expected=stamp;
  // Geometry exporters receive an owned copy of the camera, never the mutable viewport camera.
  auto camera=info.camera ? std::make_shared<Camera>(*info.camera) : std::shared_ptr<Camera>{}; info.camera=camera.get();
  auto delivery=dispatcher; QPointer<TargetService> service(this);
  (void)QtConcurrent::run([target=std::move(target),space,destination=std::move(destination),info,camera,stopped,expected,delivery,service] {
    auto staged=stageExport(target,space,destination,info,[stopped]{return stopped->load();});
    QMetaObject::invokeMethod(delivery.get(), [service,stopped,expected,staged=std::move(staged)] {
      if (!service) return; QString error;
      const bool success=commitExport(staged,[service,stopped,expected]{return service && !stopped->load() && service->validate(expected);},&error);
      emit service->exportFinished(success,success ? staged.destination : error);
    }, Qt::QueuedConnection);
  });
  return true;
}
}
