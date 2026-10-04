#include "gui/InspectionPresentationController.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/selection/EvaluationBinding.h"
#include "glview/InspectionMeshRenderer.h"
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QThread>
#include <utility>

namespace {
bool sameSource(Selection::Stamp a,Selection::Stamp b)
{
  a.viewportGeneration=b.viewportGeneration=0; return a==b;
}
}
InspectionPresentationController::Token::Token(InspectionPresentationController *controller,Kind kind,uint64_t id)
  : controller(controller),kind(kind),id(id) {}
InspectionPresentationController::Token::Token(Token&& other) noexcept
  : controller(other.controller),kind(other.kind),id(std::exchange(other.id,0)) {}
InspectionPresentationController::Token& InspectionPresentationController::Token::operator=(Token&& other) noexcept
{
  if (this!=&other) { reset(); controller=other.controller; kind=other.kind; id=std::exchange(other.id,0); }
  return *this;
}
InspectionPresentationController::Token::operator bool() const { return controller && id && controller->contains(kind,id); }
void InspectionPresentationController::Token::reset()
{
  if (controller && id) controller->release(kind,std::exchange(id,0));
  else id=0;
}
bool InspectionPresentationController::Token::setEnabled(bool enabled)
{
  if (!*this || kind!=Kind::Provider) return false;
  auto& registry=controller->registry;
  const auto old=registry.selectedProvider();
  if (!registry.enableProvider(id,enabled)) return false;
  if (old!=registry.selectedProvider()) controller->refresh();
  return true;
}
bool InspectionPresentationController::Token::setMask(const InspectionMask& mask,const Selection::Stamp& stamp,uint64_t generation)
{
  if (!*this || kind!=Kind::Request || !controller->window->evaluationBinding->validate(stamp)) return false;
  if (!controller->registry.setMask(id,mask,stamp.resultId.toStdString(),generation)) return false;
  controller->presentationChanged(); return true;
}
void InspectionPresentationController::Token::clearMask()
{
  if (*this && kind==Kind::Request) { controller->registry.clearMask(id); controller->presentationChanged(); }
}
void InspectionPresentationController::Token::uniformsChanged()
{
  if (*this && kind==Kind::Effect) controller->presentationChanged();
}
InspectionPresentationController::InspectionPresentationController(MainWindow *window) : QObject(window),window(window)
{
  // A queued dispatcher remains alive for worker completions after controller destruction.
  // QPointer is read only by its GUI-thread delivery, never from a worker.
  dispatcher=std::shared_ptr<QObject>(new QObject,[](QObject *object) { object->deleteLater(); });
  label=new QLabel(window); label->setObjectName("inspectionPreparationStatus");
  cancelButton=new QPushButton(tr("Cancel"),window); cancelButton->setObjectName("cancelInspectionPreparation");
  window->statusBar()->addPermanentWidget(label); window->statusBar()->addPermanentWidget(cancelButton);
  connect(cancelButton,&QPushButton::clicked,this,&InspectionPresentationController::cancelPreparation);
  connect(window->evaluationBinding,&Selection::EvaluationBinding::installed,this,[this] {
    QTimer::singleShot(0,this,[this] { refresh(true); });
  });
  connect(window->evaluationBinding,&Selection::EvaluationBinding::invalidated,this,[this] {
    if (replacing || !registry.requested()) return;
    stopJob(); registry.invalidate();
    setStatus(Status::Stale,tr("Inspection inputs or result changed")); emit meshesChanged();
    if (this->window->evaluationBinding->current()) QTimer::singleShot(0,this,[this] { refresh(true); });
  });
  setStatus(Status::Inactive);
}
InspectionPresentationController::~InspectionPresentationController()
{
  stopJob();
  if (window) window->qglview->makeCurrent();
  meshRenderer.reset(); defaultRenderer.reset();
}
Selection::Stamp InspectionPresentationController::stamp() const { return window->evaluationBinding->stamp(); }
uint64_t InspectionPresentationController::meshUploadCount() const { return meshRenderer ? meshRenderer->uploadCount() : 0; }
bool InspectionPresentationController::contains(Kind kind,uint64_t id) const
{
  return kind==Kind::Provider ? providers.count(id)!=0 : kind==Kind::Effect ? effects.count(id)!=0 : requests.count(id)!=0;
}
InspectionPresentationController::Token InspectionPresentationController::scoped(Kind kind,uint64_t id,QObject *lifetime)
{
  if (!id) return {};
  lifetimes[id]=connect(lifetime,&QObject::destroyed,this,[this,kind,id] { release(kind,id); });
  return Token(this,kind,id);
}
InspectionPresentationController::Token InspectionPresentationController::registerMeshProvider(std::string id,int priority,QObject *lifetime,Start start)
{
  if (!lifetime || !start) return {};
  const auto previous=registry.selectedProvider(),token=registry.registerProvider(std::move(id),priority);
  if (!token) return {};
  providers.emplace(token,Provider{lifetime,std::move(start)});
  auto result=scoped(Kind::Provider,token,lifetime);
  if (previous!=registry.selectedProvider()) refresh();
  return result;
}
InspectionPresentationController::Token InspectionPresentationController::registerMeshEffect(std::string id,int order,QObject *lifetime,Factory factory)
{
  if (!lifetime || !factory) return {};
  const auto token=registry.registerEffect(std::move(id),order);
  if (!token) return {};
  effects.emplace(token,Effect{lifetime,std::move(factory)});
  auto result=scoped(Kind::Effect,token,lifetime); compose(); return result;
}
InspectionPresentationController::Token InspectionPresentationController::requestMeshes(std::string owner,QObject *lifetime)
{
  if (!lifetime) return {};
  const bool first=!registry.requested();
  const auto token=registry.request(std::move(owner));
  if (!token) return {};
  requests.emplace(token,lifetime);
  auto result=scoped(Kind::Request,token,lifetime);
  if (first) { defaultRenderer=window->qglview->renderer; refresh(); }
  return result;
}
void InspectionPresentationController::release(Kind kind,uint64_t id)
{
  if (!contains(kind,id)) return;
  QObject::disconnect(lifetimes[id]); lifetimes.erase(id);
  if (kind==Kind::Effect) { effects.erase(id); registry.removeEffect(id); compose(); return; }
  if (kind==Kind::Provider) {
    const auto previous=registry.selectedProvider(); providers.erase(id); registry.removeProvider(id);
    if (previous!=registry.selectedProvider()) refresh();
    return;
  }
  requests.erase(id); registry.release(id);
  if (registry.requested()) { presentationChanged(); return; }
  stopJob();
  replace(defaultRenderer);
  window->qglview->makeCurrent(); meshRenderer.reset(); defaultRenderer.reset(); window->qglview->doneCurrent();
  setStatus(Status::Inactive); emit meshesChanged();
}
void InspectionPresentationController::stopJob()
{
  auto cancellation=std::move(cancel); cancel={};
  if (cancellation) cancellation();
}
void InspectionPresentationController::cancelPreparation()
{
  if (currentStatus!=Status::Preparing) return;
  stopJob(); registry.invalidate(); setStatus(Status::Cancelled,tr("Inspection preparation cancelled")); emit meshesChanged();
}
void InspectionPresentationController::refresh(bool newResult)
{
  if (!registry.requested()) return;
  stopJob(); registry.invalidate();
  if (newResult) { defaultRenderer=window->qglview->renderer; }
  if (!window->evaluationBinding->validate(stamp())) { setStatus(Status::Stale,tr("Evaluate the active document to prepare inspection")); return; }
  // Provider ownership changes invalidate geometric keys even while last-good display is retained.
  replace(window->qglview->renderer);
  const auto provider=registry.selectedProvider();
  if (!provider) { setStatus(Status::Unsupported,tr("No inspection mesh provider is available")); emit meshesChanged(); return; }
  jobStamp=stamp(); const auto scene=window->evaluationBinding->scene(jobStamp);
  const auto ticket=registry.beginJob();
  setStatus(Status::Preparing,tr("Preparing inspection")); emit meshesChanged();
  const auto delivery=dispatcher;
  const QPointer<InspectionPresentationController> weak(this);
  try {
    cancel=providers.at(provider).start(scene,jobStamp,[delivery,weak,ticket,provider](Completion result) mutable {
      QMetaObject::invokeMethod(delivery.get(),[weak,ticket,provider,result=std::move(result)]() mutable {
        if (weak) weak->complete(ticket,provider,std::move(result));
      },Qt::QueuedConnection);
    });
  } catch (const std::exception& error) { complete(ticket,provider,{Status::Failed,{},QString::fromUtf8(error.what())}); }
}
void InspectionPresentationController::complete(uint64_t ticket,uint64_t provider,Completion completion)
{
  if (!registry.accepts(ticket,provider) || !sameSource(jobStamp,stamp()) || !window->evaluationBinding->validate(stamp())) return;
  cancel={};
  if (completion.status==Status::Empty && !completion.meshes) {
    auto empty=std::make_shared<InspectionMeshSet>(); empty->resultId=stamp().resultId.toStdString(); empty->geometryGeneration=ticket;
    completion.meshes=empty;
  }
  if (completion.status==Status::Ready || completion.status==Status::Empty) {
    std::string error;
    if (!registry.install(completion.meshes,stamp().resultId.toStdString(),&error)) {
      setStatus(Status::Failed,QString::fromStdString(error)); emit meshesChanged(); return;
    }
    window->qglview->makeCurrent(); meshRenderer=std::make_shared<InspectionMeshRenderer>(registry.meshes()); window->qglview->doneCurrent();
    setStatus(registry.meshes()->pieces.empty() ? Status::Empty : Status::Ready);
    compose(); emit meshesChanged();
  } else { setStatus(completion.status,completion.reason); emit meshesChanged(); }
}
void InspectionPresentationController::compose()
{
  if (!registry.meshes() || !meshRenderer || !registry.requested()) return;
  std::shared_ptr<Renderer> composed=meshRenderer;
  try {
    for (auto id : registry.orderedEffects()) {
      composed=effects.at(id).factory(composed);
      if (!composed) throw std::runtime_error("Inspection effect returned no renderer");
    }
  } catch (const std::exception& error) { setStatus(Status::Failed,QString::fromUtf8(error.what())); return; }
  replace(std::move(composed));
}
void InspectionPresentationController::replace(std::shared_ptr<Renderer> renderer)
{
  replacing=true; window->qglview->makeCurrent();
  window->evaluationBinding->replacePresentationRenderer(std::move(renderer),stamp());
  window->qglview->doneCurrent(); replacing=false;
}
void InspectionPresentationController::presentationChanged()
{
  if (meshRenderer && registry.meshes()) meshRenderer->setVisible(registry.visible());
  emit window->qglview->presentationChanged(); window->qglview->update(); emit meshesChanged();
}
void InspectionPresentationController::setStatus(Status status,QString reason)
{
  currentStatus=status; statusReason=std::move(reason);
  label->setText(statusReason); label->setVisible(status!=Status::Inactive && status!=Status::Ready && status!=Status::Empty);
  cancelButton->setVisible(status==Status::Preparing); emit statusChanged();
}
