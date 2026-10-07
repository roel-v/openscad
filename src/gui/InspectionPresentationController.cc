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
#include <algorithm>

namespace {
bool sameSource(Selection::Stamp a,Selection::Stamp b)
{
  a.viewportGeneration=b.viewportGeneration=0; return a==b;
}
}
InspectionPresentationController::DraftToken::DraftToken(DraftToken&& other) noexcept
  :controller(other.controller),id(std::exchange(other.id,0)){}
InspectionPresentationController::DraftToken& InspectionPresentationController::DraftToken::operator=(DraftToken&& other) noexcept
{
  if(this!=&other){reset();controller=other.controller;id=std::exchange(other.id,0);}return *this;
}
InspectionPresentationController::DraftToken::operator bool() const{return controller && controller->draftCurrent(id);}
void InspectionPresentationController::DraftToken::reset(){if(controller && id)controller->releaseDraft(std::exchange(id,0));else id=0;}
bool InspectionPresentationController::DraftToken::showBase(){return controller && controller->showDraftBase(id);}
bool InspectionPresentationController::DraftToken::showCandidate(uint64_t serial,std::shared_ptr<const InspectionMeshSet> meshes,QString label)
{return controller && controller->showDraftCandidate(id,serial,std::move(meshes),std::move(label));}
bool InspectionPresentationController::DraftToken::setPending(QString label){return controller && controller->pendingDraft(id,std::move(label));}
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
  if (!*this || kind!=Kind::Request || !controller->window->evaluationBinding->validateModel(stamp)) return false;
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
  connect(cancelButton,&QPushButton::clicked,this,[this]{if(draftActive())cancelDraft();else cancelPreparation();});
  connect(window->evaluationBinding,&Selection::EvaluationBinding::installed,this,[this] {
    // Capture the newly installed native renderer before a queued consumer releases its request.
    // Sampling it in refresh() can instead retain an earlier presentation or a null restore.
    if(registry.requested())defaultRenderer=this->window->qglview->renderer;
    if(draftActive()){draftNeedsRefresh=true;releaseDraft(draftId);}
    QTimer::singleShot(0,this,[this] { refresh(); });
  });
  connect(window->evaluationBinding,&Selection::EvaluationBinding::invalidated,this,[this] {
    if(!replacing && draftActive() && !this->window->evaluationBinding->modelDisplayCurrent(draftBase))releaseDraft(draftId);
    if (replacing || !registry.requested()) return;
    stopJob(); registry.invalidate();
    setStatus(Status::Stale,tr("Inspection inputs or result changed")); emit meshesChanged();
    if (this->window->evaluationBinding->current()) QTimer::singleShot(0,this,[this] { refresh(true); });
  });
  setStatus(Status::Inactive);
}
InspectionPresentationController::~InspectionPresentationController()
{
  QObject::disconnect(draftLifetime);
  if(window && window->evaluationBinding)window->evaluationBinding->hypotheticalPixels=false;
  stopJob();
  if (window) window->qglview->makeCurrent();
  draftRenderer.reset();draftBaseRenderer.reset();meshRenderer.reset(); defaultRenderer.reset();
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
  if (first) { defaultRenderer=draftActive()?draftBaseRenderer:window->qglview->renderer; refresh(); }
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
  if(draftActive()){draftNeedsRefresh=true;return;}
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
  if(draftActive()){draftNeedsRefresh=true;return;}
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
  setStatus(Status::Preparing,tr("Preparing geometry for model tools… Large models may take a while. You can cancel this operation.")); emit meshesChanged();
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
  if(draftActive())return;
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
  if(draftActive())return;
  if (!registry.meshes() || !meshRenderer || !registry.requested()) return;
  std::shared_ptr<Renderer> composed=meshRenderer;
  meshRenderer->setColorScheme(*window->qglview->colorscheme);
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
  if(renderer)renderer->setColorScheme(*window->qglview->colorscheme);
  window->evaluationBinding->replacePresentationRenderer(std::move(renderer),stamp());
  window->qglview->doneCurrent(); replacing=false;
}
void InspectionPresentationController::presentationChanged()
{
  if (meshRenderer && registry.meshes()) meshRenderer->setVisible(registry.visible());
  if(draftActive())return;
  emit window->qglview->presentationChanged(); window->qglview->update(); emit meshesChanged();
}
void InspectionPresentationController::setStatus(Status status,QString reason)
{
  currentStatus=status; statusReason=std::move(reason);
  if(draftActive()){emit statusChanged();return;}
  label->setText(statusReason); label->setVisible(status!=Status::Inactive && status!=Status::Ready && status!=Status::Empty);
  cancelButton->setVisible(status==Status::Preparing); emit statusChanged();
}

bool InspectionPresentationController::draftCurrent(uint64_t id) const
{
  return id && id==draftId && window && window->evaluationBinding->validateModel(draftBase);
}
InspectionPresentationController::DraftToken InspectionPresentationController::beginDraft(QString owner,QObject *lifetime,
  const Selection::Stamp& base,QString *detail)
{
  if(owner.isEmpty() || !lifetime || draftActive() || !window->evaluationBinding->validate(base,detail)){
    if(detail && detail->isEmpty())*detail=tr("Another display draft is active or its base is unavailable");return {};
  }
  draftNeedsRefresh=currentStatus==Status::Preparing;stopJob();registry.beginJob();
  draftBase=base;draftBaseRenderer=registry.requested()?defaultRenderer:window->qglview->renderer;
  draftId=++nextDraftId;draftSerial=0;draftIsPending=false;
  draftLifetime=connect(lifetime,&QObject::destroyed,this,[this,id=draftId]{releaseDraft(id);});
  showDraftBase(draftId);emit draftDisplayChanged(true,false);return DraftToken(this,draftId);
}
bool InspectionPresentationController::showDraftBase(uint64_t id)
{
  if(!draftCurrent(id))return false;
  window->evaluationBinding->hypotheticalPixels=false;replace(draftBaseRenderer);
  label->setText(tr("Draft preview — not committed"));label->show();cancelButton->show();
  return true;
}
bool InspectionPresentationController::pendingDraft(uint64_t id,QString text)
{
  if(!draftCurrent(id))return false;
  draftIsPending=true;label->setText(std::move(text));label->show();cancelButton->show();emit draftDisplayChanged(true,true);return true;
}
bool InspectionPresentationController::showDraftCandidate(uint64_t id,uint64_t serial,std::shared_ptr<const InspectionMeshSet> meshes,QString text)
{
  if(!draftCurrent(id) || serial<=draftSerial || !meshes || meshes->resultId.empty() ||
     meshes->resultId==draftBase.resultId.toStdString() || meshes->geometryGeneration!=serial)return false;
  InspectionPresentation validation;std::string error;
  if(!validation.install(meshes,meshes->resultId,&error))return false;
  for(const auto& piece:meshes->pieces){
    for(const auto& vertex:piece.mesh->vertices)if(!vertex.allFinite())return false;
    for(const auto& face:piece.mesh->indices)for(auto index:face)if(index>=piece.mesh->vertices.size())return false;
  }
  window->qglview->makeCurrent();
  try{auto candidate=std::make_shared<InspectionMeshRenderer>(std::move(meshes),false);
    candidate->prepare(window->qglview->edge_shader.get());draftRenderer=std::move(candidate);
  }catch(const std::exception& error){window->qglview->doneCurrent();label->setText(QString::fromUtf8(error.what()));return false;}
  window->qglview->doneCurrent();
  window->evaluationBinding->hypotheticalPixels=true;replace(draftRenderer);
  draftSerial=serial;draftIsPending=false;label->setText(std::move(text));label->show();cancelButton->show();
  emit draftDisplayChanged(true,false);return true;
}
void InspectionPresentationController::releaseDraft(uint64_t id)
{
  if(!id || id!=draftId)return;
  QObject::disconnect(draftLifetime);draftId=0;draftIsPending=false;
  if(!window->evaluationBinding->validateModel(draftBase))draftNeedsRefresh=true;
  window->evaluationBinding->hypotheticalPixels=false;
  // A real installed result already owns its renderer; never overwrite it with the old draft base.
  if(sameSource(draftBase,stamp()))replace(draftBaseRenderer);
  window->qglview->makeCurrent();draftRenderer.reset();draftBaseRenderer.reset();window->qglview->doneCurrent();
  setStatus(currentStatus,statusReason);emit draftDisplayChanged(false,false);
  if(registry.requested()){
    if(draftNeedsRefresh || !registry.meshes())QTimer::singleShot(0,this,[this]{refresh();});else compose();
  }else{setStatus(Status::Inactive);defaultRenderer.reset();}
  draftNeedsRefresh=false;
}
void InspectionPresentationController::cancelDraft(){releaseDraft(draftId);}
