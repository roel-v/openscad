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
#include "glview/SurfacePickMath.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMouseEvent>
#include <QMenu>
#include <QOpenGLContext>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <utility>

namespace Clipping {
namespace {
Value vector(const Vector3d& v) { VectorType result(nullptr);for(int i=0;i<3;++i)result.emplace_back(Value(v[i]));return Value(std::move(result)); }
Vector3d presetNormal(const std::string& preset) {return preset=="XZ"?Vector3d::UnitY().eval():preset=="YZ"?Vector3d::UnitX().eval():Vector3d::UnitZ().eval();}
QString personalKey(EditorInterface& editor) {
  auto identity=editor.filepath.isEmpty()?editor.documentId():QFileInfo(editor.filepath).absoluteFilePath();
  return "inspection/sections/"+QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(),QCryptographicHash::Sha256).toHex())+"/";
}
std::optional<Ray> ray(MainWindow& window,QPointF logical) {
  auto view=window.qglview;view->makeCurrent();view->setupCamera();
  Eigen::Matrix4d model=Eigen::Map<const Eigen::Matrix4d>(view->modelview),projection=Eigen::Map<const Eigen::Matrix4d>(view->projection);view->doneCurrent();
  auto pixel=SurfacePickMath::pixel(logical.x(),logical.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height);if(!pixel)return {};
  auto nearPoint=SurfacePickMath::unproject(pixel->x(),pixel->y(),0,view->cam.pixel_width,view->cam.pixel_height,projection,model,Vector3d::Zero());
  auto farPoint=SurfacePickMath::unproject(pixel->x(),pixel->y(),.9999,view->cam.pixel_width,view->cam.pixel_height,projection,model,Vector3d::Zero());
  if(!nearPoint || !farPoint)return {};return Ray{*nearPoint,(*farPoint-*nearPoint).normalized()};
}
QPointF projected(const Selection::OverlayContext& context,const Vector3d& point) {
  Eigen::Vector4d clip=context.projection*context.modelview*Eigen::Vector4d(point.x(),point.y(),point.z(),1);clip/=clip.w();
  return {(clip.x()+1)*context.logicalViewport.width()/2,(1-clip.y())*context.logicalViewport.height()/2};
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
  connect(window->viewportTools,&Selection::ViewportToolRouter::feedback,this,[this](QString value){message=value;emit changed();});
  connect(window->qglview->navigation(),&CameraNavigation::displayChanged,this,[this]{if(tool){cancelGesture();refresh();}});
  connect(window->qglview,&QGLView::resized,this,[this]{if(tool){cancelGesture();refresh();}});
  overlay=window->viewportOverlays->registerOverlay(this,Selection::OverlayLayer::WorldOverlay,30,[this](const auto& context){drawHelper(context);});
  window->qglview->installEventFilter(this);bind();refresh();
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
  settingsKey=key;enabledIds.clear();helperIds.clear();
  QSettingsCached settings;global=settings.value(key+"enabled",true).toBool();selected=settings.value(key+"selected").toString().toStdString();
  for(const auto& id:settings.value(key+"enabledIds").toStringList())if(enabledIds.size()<ClipState::MaxPlanes)enabledIds.insert(id.toStdString());
  for(const auto& id:settings.value(key+"helperIds").toStringList())helperIds.insert(id.toStdString());
  documentConnection=connect(document,&Design::DesignDocument::changed,this,[this]{refresh();});
}
void Controller::savePersonal() const {
  if(settingsKey.isEmpty())return;QStringList enabled,helpers;for(const auto& id:enabledIds)enabled<<QString::fromStdString(id);for(const auto& id:helperIds)helpers<<QString::fromStdString(id);
  QSettingsCached settings;settings.setValue(settingsKey+"enabled",global);settings.setValue(settingsKey+"enabledIds",enabled);settings.setValue(settingsKey+"helperIds",helpers);settings.setValue(settingsKey+"selected",QString::fromStdString(selected));
}
Design::Records Controller::records() const {
  if(!document || !window->evaluationBinding->validate(window->evaluationBinding->stamp()))return {};
  auto all=document->records(window->evaluationBinding->stamp());
  for(const auto& [token,edit]:document->drafts())if(!edit.consumer && edit.preview)all.insert_or_assign(edit.key,edit.preview);
  return all;
}
BoundingBox Controller::bounds() const {return window->qglview->renderer?window->qglview->renderer->getBoundingBox():BoundingBox();}
Design::ResolveContext Controller::context(const Design::Records& all) const {
  auto stamp=window->evaluationBinding->stamp();return {stamp.resultId.toStdString(),stamp.sourceFingerprint.toStdString(),stamp.configurationFingerprint.toStdString(),bounds(),all,{}};
}
std::optional<Transform3d> Controller::frame(const std::string& id,QString *error) const {
  auto all=records();auto found=all.find(id);if(found==all.end()){if(error)*error=tr("Missing Section or stale evaluation");return {};}
  auto resolveContext=context(all);auto resolution=Design::sectionFrame(*found->second,resolveContext);
  if(resolution.state!=Design::ResolutionState::Resolved || !resolution.frame){if(error)*error=QString::fromStdString(resolution.diagnostic);return {};}
  return resolution.frame;
}
ClipCapabilities Controller::capabilities() const {
  ClipCapabilities result;auto view=window->qglview;
  result.available=view->isValid() && view->context() && view->context()->format().majorVersion()>=3 && view->edge_shader && view->edge_shader->resource.shader_program;
  result.reason=result.available?"Caps are unavailable; live Sections display exposed mesh surfaces":"Live Sections require a shader-capable viewport";return result;
}
void Controller::fail(const QString& detail){message=detail;emit changed();}
void Controller::refresh() {
  if(refreshing || closing)return;refreshing=true;
  if(!window->evaluationBinding->current()){message=tr("Evaluate the active document to edit Sections. The last evaluated display is retained.");refreshing=false;emit changed();return;}
  bind();auto all=records();if(selected.empty() || !all.count(selected)){selected.clear();for(const auto& [id,record]:all)if(record->kind=="Section"){selected=id;break;}}
  bool wanted=global && !enabledIds.empty();
  if(wanted && !provider)provider=window->inspectionPresentation->registerMeshProvider("live-sections-root",0,this,[this](auto scene,auto stamp,auto done){return jobs->start(scene,stamp,done);});
  if(wanted && !effect)effect=window->inspectionPresentation->registerMeshEffect("live-sections",20,this,[this](auto child) {
    auto renderer=std::make_shared<ClippedMeshRenderer>(child,state);auto view=window->qglview;view->makeCurrent();
    try{renderer->prepare(view->edge_shader.get());}catch(...){renderer.reset();view->doneCurrent();throw;}view->doneCurrent();return renderer;
  });
  if(wanted && !request)request=window->inspectionPresentation->requestMeshes("live-sections",this);
  if(!wanted){state->planes.clear();effect.reset();request.reset();provider.reset();}
  auto box=bounds();std::vector<ClipPlane> planes;QString problems;
  if(wanted && !box.isEmpty())for(const auto& id:enabledIds){QString error;auto resolved=frame(id,&error);if(resolved)try{planes.push_back(prepareClipPlane(resolved->translation(),resolved->linear().col(2),box,id));}catch(const std::exception& exception){error=QString::fromUtf8(exception.what());}if(!error.isEmpty())problems+=QString::fromStdString(id)+": "+error+"\n";}
  state->planes=std::move(planes);++state->generation;
  if(effect)effect.uniformsChanged();
  auto presentation=window->inspectionPresentation;
  if(!problems.isEmpty())message=problems.trimmed();
  else if(wanted && presentation->status()!=InspectionPresentationController::Status::Ready && presentation->status()!=InspectionPresentationController::Status::Empty)message=presentation->reason();
  else if(wanted && box.isEmpty())message=tr("The evaluated model is empty. No clipping fragments are available.");
  else message=tr("Caps are unavailable. Helpers show the plane and retained direction.");
  refreshing=false;emit changed();window->qglview->update();
}
std::optional<std::string> Controller::createPreset(const std::string& preset) {
  if(!document || !window->evaluationBinding->current()){fail(tr("Evaluate the document first"));return {};}
  if(bounds().isEmpty()){fail(tr("A preset requires model bounds"));return {};}
  ObjectType definition(nullptr);definition.set("label",Value(preset+" Section"));definition.set("origin",vector(bounds().center()));definition.set("normal",vector(presetNormal(preset)));definition.set("up",vector(transportedUp(presetNormal(preset),Vector3d::UnitZ())));
  std::string error;auto id=document->createTransient("Section",Value(std::move(definition)),&error);if(!id){fail(QString::fromStdString(error));return {};}
  selected=*id;setHelperEnabled(*id,true);enable(*id,true);refresh();return id;
}
bool Controller::enable(const std::string& id,bool value) {
  if(value){QString error;if(!frame(id,&error)){fail(error);return false;}if(!enabledIds.count(id) && enabledIds.size()>=ClipState::MaxPlanes){fail(tr("At most eight Sections can be enabled. Disable another Section first."));return false;}if(!capabilities().available){fail(QString::fromStdString(capabilities().reason));return false;}enabledIds.insert(id);}
  else {enabledIds.erase(id);if(enabledIds.empty()){state->planes.clear();effect.reset();request.reset();provider.reset();}}
  savePersonal();refresh();return true;
}
void Controller::setGlobalEnabled(bool value){cancelGesture();global=value;if(!global){state->planes.clear();effect.reset();request.reset();provider.reset();}savePersonal();refresh();}
void Controller::setHelperEnabled(const std::string& id,bool value){if(value)helperIds.insert(id);else helperIds.erase(id);savePersonal();emit changed();window->qglview->update();}
void Controller::select(const std::string& id){cancelGesture();selected=id;savePersonal();emit changed();window->qglview->update();}
bool Controller::editable(const std::string& id,QString *reason) const {
  if(GuiLocker::isLocked()){if(reason)*reason=tr("Wait for inspection preparation to finish, or cancel it.");return false;}
  auto all=records();auto found=all.find(id);if(found==all.end()){if(reason)*reason=tr("Missing Section or stale evaluation");return false;}
  if(!frame(id,reason))return false;
  if(found->second->source){auto section=std::dynamic_pointer_cast<const Design::Section>(found->second->evaluated);if(!section)return false;Design::FieldChange change{Design::ChangeKind::Offset};change.offset=section->offset;
    auto prepared=Design::DesignEdits(window).prepare(*found->second,{change},window->evaluationBinding->stamp());if(!prepared){if(reason)*reason=QString::fromStdString(prepared.error);return false;}}
  return true;
}
std::optional<std::pair<double,double>> Controller::offsetRange(const std::string& id) const {
  auto all=records();auto found=all.find(id);auto pose=frame(id);auto box=bounds();if(found==all.end() || !pose || box.isEmpty())return {};
  auto section=std::dynamic_pointer_cast<const Design::Section>(found->second->evaluated);if(!section)return {};
  double lower=INFINITY,upper=-INFINITY;for(int corner=0;corner<8;++corner){double parameter=(box.corner(static_cast<BoundingBox::CornerType>(corner))-pose->translation()).dot(pose->linear().col(2))+section->offset;lower=std::min(lower,parameter);upper=std::max(upper,parameter);}return std::pair{lower,upper};
}
bool Controller::beginGesture(const std::string& id) {
  cancelGesture();QString error;if(!editable(id,&error)){fail(error);return false;}gestureStamp=window->evaluationBinding->stamp();auto token=document->beginEdit(id,gestureStamp);if(!token){fail(tr("Section already has an active edit"));return false;}draft=*token;priorGesture=*state;return true;
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
  auto resolveContext=context(all);auto parent=Design::resolve(section->frame,resolveContext);if(parent.state!=Design::ResolutionState::Resolved || !parent.frame)return {};
  Vector3d localPoint=parent.frame->inverse()*point,localNormal=parent.frame->linear().transpose()*normal,localUp=parent.frame->linear().transpose()*up;
  Design::FieldChange rotation{Design::ChangeKind::Rotation};rotation.rotation=orientation(section->normal,section->up,localNormal,localUp);
  Design::FieldChange translation{Design::ChangeKind::Translation};translation.translation=localPoint-section->origin-section->offset*(rotation.rotation*section->normal);return {rotation,translation};
}
bool Controller::flip(const std::string& id){auto pose=frame(id);if(!pose)return false;return edit(id,poseChanges(id,pose->translation(),-pose->linear().col(2),pose->linear().col(1)));}
bool Controller::reset(const std::string& id,const std::string& preset){auto pose=frame(id);if(!pose)return false;auto normal=presetNormal(preset);return edit(id,poseChanges(id,pose->translation(),normal,transportedUp(normal,pose->linear().col(1))));}
bool Controller::setAngles(const std::string& id,double azimuth,double elevation){auto pose=frame(id);if(!pose || !std::isfinite(azimuth) || !std::isfinite(elevation))return false;double radians=std::acos(-1.)/180;azimuth*=radians;elevation*=radians;Vector3d normal(std::cos(elevation)*std::cos(azimuth),std::cos(elevation)*std::sin(azimuth),std::sin(elevation));return edit(id,poseChanges(id,pose->translation(),normal,transportedUp(normal,pose->linear().col(1))));}
bool Controller::setOrigin(const std::string& id,const Vector3d& origin){auto pose=frame(id);if(!pose || !origin.allFinite())return false;return edit(id,poseChanges(id,origin,pose->linear().col(2),pose->linear().col(1)));}
bool Controller::remove(const std::string& id){cancelGesture();auto result=document?document->remove(id,window->evaluationBinding->stamp()):Design::LifecycleResult{false,"Document closed"};if(!result){fail(QString::fromStdString(result.error));return false;}refresh();return true;}
bool Controller::promote(const std::string& id){cancelGesture();if(!owner || !document)return false;auto result=document->promote({id},*owner,window->evaluationBinding->stamp());if(!result){fail(QString::fromStdString(result.error));return false;}refresh();return true;}
bool Controller::applyHit(const std::string& id,const Selection::Hit& hit,bool align) {
  if(!window->evaluationBinding->validate(hit.stamp)){fail(tr("Pick belongs to a stale result"));return false;}auto pose=frame(id);if(!pose)return false;
  Vector3d normal=align?-hit.outwardNormal:pose->linear().col(2).eval();return edit(id,poseChanges(id,hit.worldPoint,normal,transportedUp(normal,pose->linear().col(1))));
}
void Controller::pick(const std::string& id,bool align) {
  cancelGesture();QString error;if(!editable(id,&error)){fail(error);return;}auto stamp=window->evaluationBinding->stamp();
  tool=window->viewportTools->beginPick({"Live Sections",stamp,true},this,[this,id,align](auto result){tool=0;if(result.hit)applyHit(id,*result.hit,align);else if(result.error!=Selection::PickError::Cancelled)fail(result.detail);});
  message=tr("Click a surface. A miss leaves picking active; Escape cancels.");emit changed();
}
bool Controller::beginDrag(const std::string& id,QPointF press,bool rotate,int axis) {
  auto pose=frame(id);auto firstRay=ray(*window,press);if(!pose || !firstRay)return false;
  auto all=records();auto record=all.find(id);if(record==all.end())return false;auto section=std::dynamic_pointer_cast<const Design::Section>(record->second->evaluated);if(!section)return false;const double baseOffset=section->offset;
  axis=std::clamp(axis,0,1);
  auto first=rotate?ringAngle(*firstRay,pose->translation(),pose->linear().col(axis),pose->linear().col(2)):axisParameter(*firstRay,pose->translation(),pose->linear().col(2));
  bool fallback=false;QPointF screenAxis;double size=bounds().isEmpty()?1:bounds().sizes().norm()*.18;
  if(!first && !rotate){auto view=window->qglview;Selection::OverlayContext context{window->evaluationBinding->stamp(),Eigen::Map<const Eigen::Matrix4d>(view->modelview),Eigen::Map<const Eigen::Matrix4d>(view->projection),view->size(),view->devicePixelRatio()};screenAxis=projected(context,pose->translation()+size*pose->linear().col(2))-projected(context,pose->translation());if(QPointF::dotProduct(screenAxis,screenAxis)>=64){fallback=true;first=0;}}
  if(!first){fail(tr("The handle is parallel to this view. Change the camera to drag it."));return false;}
  if(!beginGesture(id))return false;auto initial=*pose;
  auto move=[this,id,initial,first=*first,rotate,axis,baseOffset,fallback,screenAxis,size,press](QPointF point,Qt::KeyboardModifiers modifiers){auto current=ray(*window,point);if(!current)return;auto parameter=fallback?std::optional<double>(size*QPointF::dotProduct(point-press,screenAxis)/QPointF::dotProduct(screenAxis,screenAxis)):rotate?ringAngle(*current,initial.translation(),initial.linear().col(axis),initial.linear().col(2)):axisParameter(*current,initial.translation(),initial.linear().col(2));if(!parameter)return;
    double delta=*parameter-first;Vector3d origin=initial.translation(),normal=initial.linear().col(2),up=initial.linear().col(1);
    if(rotate){delta=std::remainder(delta,2*std::acos(-1.));Eigen::AngleAxisd rotation(snappedAngle(delta,modifiers.testFlag(Qt::ShiftModifier)),initial.linear().col(axis));normal=rotation*normal;up=rotation*up;preview(poseChanges(id,origin,normal,up));}
    else{Design::FieldChange offset{Design::ChangeKind::Offset};offset.offset=baseOffset+delta;preview({offset});}};
  tool=window->viewportTools->beginDrag({"Live Sections",gestureStamp,true},this,press,{move,[this,move](auto point,auto modifiers){move(point,modifiers);tool=0;finishGesture();},[this](auto){tool=0;cancelGesture();refresh();}});
  if(!tool){cancelGesture();return false;}return true;
}
void Controller::drawHelper(const Selection::OverlayContext&) {
  if(!window->evaluationBinding->current())return;auto pose=frame(selected);if(!pose || !helperEnabled(selected))return;
  double size=bounds().isEmpty()?1:bounds().sizes().norm()*.18;Vector3d origin=pose->translation(),normal=pose->linear().col(2),up=pose->linear().col(1),right=pose->linear().col(0);
  glDisable(GL_LIGHTING);glColor4f(.1f,.7f,1.f,1.f);glLineWidth(2);glBegin(GL_LINES);
  auto line=[](const Vector3d& a,const Vector3d& b){glVertex3dv(a.data());glVertex3dv(b.data());};
  line(origin,origin+normal*size);line(origin-right*size,origin+right*size);line(origin-up*size,origin+up*size);glEnd();
  glBegin(GL_LINE_LOOP);for(const Vector3d& corner:std::array<Vector3d,4>{-right-up,right-up,right+up,-right+up}){Vector3d point=origin+size*corner;glVertex3dv(point.data());}glEnd();
  glBegin(GL_LINES);line(origin+normal*size,origin+normal*(size*.8)+right*(size*.1));line(origin+normal*size,origin+normal*(size*.8)-right*(size*.1));glEnd();
  for(const auto& axis:{right,up}){glBegin(GL_LINE_LOOP);for(int i=0;i<64;++i){double angle=i*2*std::acos(-1.)/64;Vector3d point=origin+size*(std::cos(angle)*normal+std::sin(angle)*axis);glVertex3dv(point.data());}glEnd();}
  glPointSize(7);glBegin(GL_POINTS);glVertex3dv(origin.data());glEnd();
}
bool Controller::eventFilter(QObject *,QEvent *event) {
  if(event->type()!=QEvent::MouseButtonPress || window->viewportTools->active() || !helperEnabled(selected))return false;
  auto& mouse=*static_cast<QMouseEvent *>(event);if(mouse.button()!=Qt::LeftButton || mouse.modifiers()!=Qt::NoModifier)return false;auto pose=frame(selected);if(!pose || !editable(selected))return false;
  auto view=window->qglview;view->makeCurrent();view->setupCamera();Selection::OverlayContext context{window->evaluationBinding->stamp(),Eigen::Map<const Eigen::Matrix4d>(view->modelview),Eigen::Map<const Eigen::Matrix4d>(view->projection),view->size(),view->devicePixelRatio()};view->doneCurrent();
  double size=bounds().isEmpty()?1:bounds().sizes().norm()*.18;auto origin=projected(context,pose->translation()),tip=projected(context,pose->translation()+size*pose->linear().col(2));
  if((mouse.position()-tip).manhattanLength()<12 || (mouse.position()-origin).manhattanLength()<9)return beginDrag(selected,mouse.position(),false);
  for(int axis=0;axis<2;++axis)for(int i=0;i<64;++i){double angle=i*2*std::acos(-1.)/64;auto point=projected(context,pose->translation()+size*(std::cos(angle)*pose->linear().col(2)+std::sin(angle)*pose->linear().col(1-axis)));if((mouse.position()-point).manhattanLength()<9)return beginDrag(selected,mouse.position(),true,axis);}
  return false;
}
}
