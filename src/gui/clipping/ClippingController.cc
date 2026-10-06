#include "gui/clipping/ClippingController.h"
#include <array>
#include "gui/clipping/ClippingDock.h"
#include "gui/clipping/SectionManipulator.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/Editor.h"
#include "gui/TabManager.h"
#include "gui/QSettingsCached.h"
#include "glview/ClippedMeshRenderer.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMenu>
#include <QOpenGLContext>
#include <algorithm>
#include <cmath>
#include <utility>

namespace Clipping {
namespace {
Value vector(const Vector3d& v) { VectorType result(nullptr);for(int i=0;i<3;++i)result.emplace_back(Value(v[i]));return Value(std::move(result)); }
QString personalKey(EditorInterface& editor) {
  auto identity=editor.filepath.isEmpty()?editor.documentId():QFileInfo(editor.filepath).absoluteFilePath();
  return "inspection/sections/"+QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(),QCryptographicHash::Sha256).toHex())+"/";
}
}
Controller::Controller(MainWindow *window) : QObject(window),window(window),jobs(new ClipMeshJob(window)) {
  setObjectName("clippingController");dock=new Dock(this,window);window->addDockWidget(Qt::RightDockWidgetArea,dock);
  window->menuWindow->addAction(dock->toggleViewAction());dock->hide();
  connect(window->evaluationBinding,&Selection::EvaluationBinding::installed,this,[this]{bind();refresh();});
  connect(window->evaluationBinding,&Selection::EvaluationBinding::invalidated,this,[this]{cancelGesture();refresh();});
  connect(window->tabManager,&TabManager::currentEditorChanged,this,[this](auto){cancelGesture();bind();refresh();});
  connect(window->inspectionPresentation,&InspectionPresentationController::statusChanged,this,[this]{refresh();});
  connect(window->inspectionPresentation,&InspectionPresentationController::meshesChanged,this,[this]{refresh();});
  connect(window->inspectionPresentation,&InspectionPresentationController::draftDisplayChanged,this,[this]{refresh();});
  connect(window->viewportTools,&Selection::ViewportToolRouter::feedback,this,[this](QString value){message=value;emit changed();});
  connect(window->qglview->navigation(),&CameraNavigation::displayChanged,this,[this]{if(tool){cancelGesture();refresh();}});
  connect(window->qglview,&QGLView::resized,this,[this]{if(tool){cancelGesture();refresh();}});
  connect(this,&Controller::changed,this,&Controller::prepareHelper);
  overlay=window->viewportOverlays->registerOverlay(this,Selection::OverlayLayer::WorldOverlay,30,[this](const auto& context){drawHelper(context);});
  bind();refresh();
}
Controller::~Controller() {
  closing=true;cancelGesture();overlay.reset();effect.reset();request.reset();provider.reset();savePersonal();
  delete dock;dock=nullptr;delete jobs;jobs=nullptr;
}
Controller *Controller::forWindow(MainWindow& window) {
  if(auto *controller=window.findChild<Controller *>(QString(),Qt::FindDirectChildrenOnly))return controller;
  return new Controller(&window);
}
void Controller::bind() {
  auto editor=window->activeEditor;if(!editor)return;
  auto key=personalKey(*editor);
  if(owner==editor && document && ownerDocumentId==editor->documentId()) {if(settingsKey!=key){settingsKey=key;savePersonal();}return;}
  savePersonal();state->planes.clear();effect.reset();request.reset();provider.reset();disconnect(documentConnection);owner=editor;ownerDocumentId=editor->documentId();document=Design::DesignDocument::forEditor(*editor,*window);
  settingsKey=key;enabledIds.clear();helperIds.clear();sessionIds.clear();
  QSettingsCached settings;global=settings.value(key+"enabled",true).toBool();selected=settings.value(key+"selected").toString().toStdString();
  for(const auto& id:settings.value(key+"enabledIds").toStringList())if(enabledIds.size()<ClipState::MaxPlanes)enabledIds.insert(id.toStdString());
  for(const auto& id:settings.value(key+"helperIds").toStringList())helperIds.insert(id.toStdString());
  documentConnection=connect(document,&Design::DesignDocument::changed,this,[this]{refresh();});
}
void Controller::savePersonal() const {
  if(settingsKey.isEmpty())return;
  // Session-only definitions are not serialized: do not retain dangling IDs for them.
  auto sessionOnly=[this](const std::string& id){return sessionIds.count(id) || (document && document->transientRecords().count(id));};
  QStringList enabled,helpers;
  for(const auto& id:enabledIds)if(!sessionOnly(id))enabled<<QString::fromStdString(id);
  for(const auto& id:helperIds)if(!sessionOnly(id))helpers<<QString::fromStdString(id);
  QSettingsCached settings;settings.setValue(settingsKey+"enabled",global);settings.setValue(settingsKey+"enabledIds",enabled);settings.setValue(settingsKey+"helperIds",helpers);settings.setValue(settingsKey+"selected",sessionOnly(selected)?QString():QString::fromStdString(selected));
}
Design::Records Controller::records() const {
  if(!document || !owner || owner!=window->activeEditor)return {};
  auto all=document->records(window->evaluationBinding->stamp());
  for(const auto& [token,edit]:document->drafts())if(!edit.consumer && edit.preview)all.insert_or_assign(edit.key,edit.preview);
  return all;
}
bool Controller::evaluationCurrent() const {return window->evaluationBinding->current();}
Design::Records Controller::displayRecords() const {
  auto result=records();
  if(document)for(const auto& item:document->sourceDisplayRecords())result.try_emplace(item.first,item.second);
  return result;
}
bool Controller::sourceStateCurrent() const {return document && document->sourceStateCurrent();}
BoundingBox Controller::bounds() const {return (evaluationCurrent() || sourceStateCurrent()) && window->qglview->renderer?window->qglview->renderer->getBoundingBox():BoundingBox();}
Design::ResolveContext Controller::context(const Design::Records& all) const {
  auto stamp=window->evaluationBinding->stamp();return {stamp.resultId.toStdString(),stamp.sourceFingerprint.toStdString(),stamp.configurationFingerprint.toStdString(),bounds(),all,{}};
}
std::optional<Transform3d> Controller::frame(const std::string& id,QString *error) const {
  auto all=records();auto found=all.find(id);if(found==all.end()){if(error)*error=tr("Missing cross section or stale evaluation");return {};}
  auto resolveContext=context(all);auto resolution=Design::sectionFrame(*found->second,resolveContext);
  if(resolution.state!=Design::ResolutionState::Resolved || !resolution.frame){if(error)*error=QString::fromStdString(resolution.diagnostic);return {};}
  return resolution.frame;
}
ClipCapabilities Controller::capabilities() const {
  ClipCapabilities result;auto view=window->qglview;
  result.available=view->isValid() && view->context() && view->context()->format().majorVersion()>=3 && view->edge_shader && view->edge_shader->resource.shader_program;
  result.reason=result.available?"Caps are unavailable; cross sections display exposed mesh surfaces":"Cross section clipping requires a shader-capable viewport";return result;
}
void Controller::fail(const QString& detail){message=detail;emit changed();}
void Controller::refresh() {
  if(refreshing || closing || (document && document->sourceEditInProgress()))return;refreshing=true;
  bind();auto all=records();auto displayed=displayRecords();if(selected.empty() || !displayed.count(selected)){selected.clear();for(const auto& [id,record]:displayed)if(record->kind=="Section"){selected=id;break;}}
  for(const auto& [id,record]:all)if(record->source)sessionIds.erase(id);else sessionIds.insert(id);
  // Keep an existing display pipeline while editing source against the last preview.
  // Picking still requires a current evaluation; retaining pixels does not validate it.
  const bool retainDisplay=sourceStateCurrent() && bool(effect);
  bool wanted=(window->evaluationBinding->current() || sourceStateCurrent()) && global && !enabledIds.empty();
  if(wanted && !provider)provider=window->inspectionPresentation->registerMeshProvider("live-sections-root",0,this,[this](auto scene,auto stamp,auto done){return jobs->start(scene,stamp,done);});
  if(wanted && !effect)effect=window->inspectionPresentation->registerMeshEffect("live-sections",20,this,[this](auto child) {
    auto renderer=std::make_shared<ClippedMeshRenderer>(child,state);auto view=window->qglview;view->makeCurrent();
    try{renderer->prepare(view->edge_shader.get());}catch(...){renderer.reset();view->doneCurrent();throw;}view->doneCurrent();return renderer;
  });
  if(wanted && !request)request=window->inspectionPresentation->requestMeshes("live-sections",this);
  if(!wanted){state->planes.clear();if(!retainDisplay){effect.reset();request.reset();provider.reset();}}
  auto box=bounds();std::vector<ClipPlane> planes;QString problems;
  if(wanted && !box.isEmpty())for(const auto& id:enabledIds){QString error;auto resolved=frame(id,&error);if(resolved)try{planes.push_back(prepareClipPlane(resolved->translation(),resolved->linear().col(2),box,id));}catch(const std::exception& exception){error=QString::fromUtf8(exception.what());}if(!error.isEmpty())problems+=QString::fromStdString(id)+": "+error+"\n";}
  state->planes=std::move(planes);++state->generation;
  if(effect)effect.uniformsChanged();
  auto presentation=window->inspectionPresentation;
  if(!problems.isEmpty())message=problems.trimmed();
  else if(wanted && evaluationCurrent() && presentation->status()!=InspectionPresentationController::Status::Ready && presentation->status()!=InspectionPresentationController::Status::Empty)message=presentation->reason();
  else message.clear();
  refreshing=false;emit changed();window->qglview->update();
}
std::optional<std::string> Controller::createSection() {
  bind();
  if(!owner || owner!=window->activeEditor || !document){fail(tr("Open a document to add a cross section."));return {};}
  // New sections remove Above (+Z); stored normals point toward the retained side.
  ObjectType definition(nullptr);definition.set("label",Value(tr("Cross section").toStdString()));definition.set("origin",vector(Vector3d::Zero()));definition.set("normal",vector(-Vector3d::UnitZ()));definition.set("up",vector(Vector3d::UnitY()));
  std::string error;auto id=document->createTransient("Section",Value(std::move(definition)),&error);if(!id){fail(QString::fromStdString(error));return {};}
  selected=*id;setHelperEnabled(*id,true);enable(*id,true);refresh();return id;
}
bool Controller::enable(const std::string& id,bool value) {
  if(value){QString error;if(!frame(id,&error)){fail(error);return false;}if(!enabledIds.count(id) && enabledIds.size()>=ClipState::MaxPlanes){fail(tr("At most eight cross sections can be enabled. Disable another cross section first."));return false;}if(!capabilities().available){fail(QString::fromStdString(capabilities().reason));return false;}enabledIds.insert(id);}
  else {enabledIds.erase(id);if(enabledIds.empty()){state->planes.clear();if(!sourceStateCurrent()){effect.reset();request.reset();provider.reset();}}}
  savePersonal();refresh();return true;
}
void Controller::setGlobalEnabled(bool value){cancelGesture();global=value;if(!global){state->planes.clear();if(!sourceStateCurrent()){effect.reset();request.reset();provider.reset();}}savePersonal();refresh();}
void Controller::setHelperEnabled(const std::string& id,bool value){if(value)helperIds.insert(id);else helperIds.erase(id);savePersonal();emit changed();window->qglview->update();}
void Controller::select(const std::string& id){cancelGesture();selected=id;savePersonal();emit changed();window->qglview->update();}
bool Controller::editable(const std::string& id,QString *reason) const {
  if(id.empty())return false;
  if(window->inspectionPresentation->draftActive()){
    if(reason)*reason=tr("Finish or cancel the insertion preview before editing cross sections.");
    return false;
  }

  if(GuiLocker::isLocked()){
    if(reason)*reason=window->inspectionPresentation->status()==InspectionPresentationController::Status::Preparing
      ? tr("Preparing geometry for cross sections. Wait, or use Cancel in the status bar. You do not need to close Inspector.")
      : tr("The model is being evaluated. Wait before editing cross sections.");
    return false;
  }
  auto all=records();auto found=all.find(id);if(found==all.end()){if(reason)*reason=tr("Source or model inputs changed. Preview to refresh the cross section controls.");return false;}
  if(!frame(id,reason))return false;
  if(found->second->source){auto section=std::dynamic_pointer_cast<const Design::Section>(found->second->evaluated);if(!section)return false;Design::FieldChange change{Design::ChangeKind::Offset};change.offset=section->offset;
    auto prepared=document->prepareSourceEdit(*found->second,{change});if(!prepared){if(reason)*reason=QString::fromStdString(prepared.error);return false;}}
  return true;
}
bool Controller::beginGesture(const std::string& id) {
  cancelGesture();QString error;if(!editable(id,&error)){fail(error);return false;}gestureStamp=window->evaluationBinding->stamp();auto token=document->beginEdit(id,gestureStamp);if(!token){fail(tr("Cross section already has an active edit"));return false;}draft=*token;priorGesture=*state;return true;
}
bool Controller::preview(const std::vector<Design::FieldChange>& changes) {
  if(draft.empty() || !document)return false;auto result=document->updateDraft(draft,changes);if(!result){fail(QString::fromStdString(result.error));cancelGesture();return false;}refresh();return true;
}
bool Controller::finishGesture() {
  if(draft.empty() || !document)return false;auto token=std::exchange(draft,{});priorGesture.reset();auto result=document->commitDraft(token);if(!result){document->cancelDraft(token);fail(QString::fromStdString(result.error));refresh();return false;}refresh();return true;
}
void Controller::cancelGesture() {
  if(tool && window->viewportTools){auto token=std::exchange(tool,0);window->viewportTools->cancel(token,Selection::CancelReason::OwnerDestroyed);}
  if(!draft.empty() && document){auto token=std::exchange(draft,{});document->cancelDraft(token);}
  if(priorGesture){*state=*priorGesture;priorGesture.reset();if(effect)effect.uniformsChanged();}
}
bool Controller::edit(const std::string& id,const std::vector<Design::FieldChange>& changes){return beginGesture(id) && preview(changes) && finishGesture();}
std::vector<Design::FieldChange> Controller::poseChanges(const std::string& id,const Vector3d& point,const Vector3d& normal,const Vector3d& up) const {
  auto all=records();auto found=all.find(id);if(found==all.end())return {};auto section=std::dynamic_pointer_cast<const Design::Section>(found->second->evaluated);if(!section)return {};
  Design::FieldChange rotation{Design::ChangeKind::Rotation};rotation.rotation=orientation(section->normal,section->up,normal,up);
  // sectionFrame adds the offset along the rotated normal. Compensate translation so
  // the resolved origin stays at point, including when a direction edit rotates that offset.
  Design::FieldChange translation{Design::ChangeKind::Translation};translation.translation=point-section->origin-section->offset*(rotation.rotation*section->normal);return {rotation,translation};
}
bool Controller::flip(const std::string& id){auto pose=frame(id);if(!pose)return false;return edit(id,poseChanges(id,pose->translation(),-pose->linear().col(2),pose->linear().col(1)));}
bool Controller::setCutAwayDirection(const std::string& id,const Vector3d& direction) {
  const double scale=direction.cwiseAbs().maxCoeff();
  if(!direction.allFinite() || !(scale>0)) {
    fail(tr("Cut away direction must be a finite, nonzero vector."));return false;
  }
  auto pose=frame(id);if(!pose)return false;
  // Stored normals point into the retained half-space; the UI points into the removed side.
  const Vector3d scaled=direction/scale;
  const Vector3d normal=-scaled/scaled.norm();
  return edit(id,poseChanges(id,pose->translation(),normal,transportedUp(normal,pose->linear().col(1))));
}
bool Controller::setOrigin(const std::string& id,const Vector3d& origin){auto pose=frame(id);if(!pose || !origin.allFinite())return false;return edit(id,poseChanges(id,origin,pose->linear().col(2),pose->linear().col(1)));}
bool Controller::remove(std::string id) {
  cancelGesture();
  const auto all=records();
  if(all.count(id)){
    auto result=document->remove(id,window->evaluationBinding->stamp());
    if(!result){fail(QString::fromStdString(result.error));return false;}
  } else if(!enabledIds.count(id) && !helperIds.count(id)){
    fail(tr("No cross section or remembered entry to remove"));return false;
  }
  enabledIds.erase(id);helperIds.erase(id);if(selected==id)selected.clear();
  std::erase_if(state->planes,[&id](const ClipPlane& plane){return plane.sectionId==id;});
  ++state->generation;
  if(enabledIds.empty() && !sourceStateCurrent()){effect.reset();request.reset();provider.reset();}
  else if(effect)effect.uniformsChanged();
  savePersonal();refresh();return true;
}
bool Controller::promote(const std::string& id){cancelGesture();if(!owner || !document)return false;auto result=document->promote({id},*owner,window->evaluationBinding->stamp());if(!result){fail(QString::fromStdString(result.error));return false;}refresh();return true;}
bool Controller::applyHit(const std::string& id,const Selection::Hit& hit,bool align) {
  // Copy world-space coordinates once; never retain the hit's geometry identity.
  if(!window->evaluationBinding->validate(hit.stamp)){fail(tr("Pick belongs to a stale result"));return false;}auto pose=frame(id);if(!pose)return false;
  Vector3d normal=align?-hit.outwardNormal:pose->linear().col(2).eval();return edit(id,poseChanges(id,hit.worldPoint,normal,transportedUp(normal,pose->linear().col(1))));
}
void Controller::pick(const std::string& id,bool align) {
  cancelGesture();QString error;if(!editable(id,&error)){fail(error);return;}auto stamp=window->evaluationBinding->stamp();
  tool=window->viewportTools->beginPick({"Cross sections",stamp,true},this,[this,id,align](auto result){tool=0;if(result.hit)applyHit(id,*result.hit,align);else if(result.error!=Selection::PickError::Cancelled)fail(result.detail);});
  message=tr("Click a surface. A miss leaves picking active; Escape cancels.");emit changed();
}
void Controller::prepareHelper() {
  helperPose.reset();
  if(!helperEnabled(selected))return;
  helperPose=frame(selected);const auto box=bounds();planeSize=box.isEmpty()?10:box.sizes().norm()*.18;
  if(helperPose && !box.isEmpty())for(int corner=0;corner<8;++corner){
    const Vector3d local=helperPose->inverse()*box.corner(static_cast<BoundingBox::CornerType>(corner));
    planeSize=std::max(planeSize,1.1*std::max(std::abs(local.x()),std::abs(local.y())));
  }
}
void Controller::drawHelper(const Selection::OverlayContext& context) {
  if(!helperPose)return;const auto& pose=helperPose;
  Vector3d origin=pose->translation(),up=pose->linear().col(1),right=pose->linear().col(0);
  glDisable(GL_LIGHTING);glDisable(GL_CULL_FACE);glDisable(GL_TEXTURE_2D);glDisable(GL_COLOR_LOGIC_OP);
  glDisable(GL_TEXTURE_1D);glDisable(GL_TEXTURE_3D);glDisable(GL_TEXTURE_CUBE_MAP);glDisable(GL_TEXTURE_RECTANGLE);
  glDisable(GL_ALPHA_TEST);glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
  // GLView leaves opaque alpha for Qt composition. Blend RGB without reducing it.
  glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ZERO,GL_ONE);
  glColor4f(.1f,.7f,1.f,.18f);glBegin(GL_QUADS);
  for(const Vector3d& corner:std::array<Vector3d,4>{-right-up,right-up,right+up,-right+up}){Vector3d point=origin+planeSize*corner;glVertex3dv(point.data());}glEnd();
  glColor4f(.1f,.7f,1.f,1.f);glLineWidth(2*context.dpr);glBegin(GL_LINE_LOOP);
  for(const Vector3d& corner:std::array<Vector3d,4>{-right-up,right-up,right+up,-right+up}){Vector3d point=origin+planeSize*corner;glVertex3dv(point.data());}glEnd();
  // A non-interactive origin marker uses the same color as the plane border.
  // ViewportOverlays scopes and restores all compatibility GL state for this callback.
  glPointSize(7*context.dpr);glEnable(GL_POINT_SMOOTH);
  glBegin(GL_POINTS);glVertex3dv(origin.data());glEnd();
}
}
