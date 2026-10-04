#include "gui/semantic/SemanticController.h"
#include "gui/semantic/SemanticMeshJob.h"
#include "gui/semantic/SemanticParameterPanel.h"
#include "gui/ExportOptions.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/selection/ProvenanceService.h"
#include "gui/design/DesignDocument.h"
#include "core/design/DesignKinds.h"
#include "core/node.h"
#include <algorithm>
#include <QDockWidget>
#include <QTreeView>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QMenu>
#include <QStatusBar>
#include <QFileDialog>
#include <QDialog>
#include <QComboBox>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QTimer>
#include <QScopedValueRollback>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
namespace Semantic {
namespace {
bool rigidFrame(const Transform3d& frame) {
  const auto linear=frame.linear();
  return frame.matrix().allFinite() && (linear.transpose()*linear).isIdentity(1e-9) && std::abs(linear.determinant()-1)<1e-9;
}
}
Controller::Controller(MainWindow *owner) : QObject(owner),window(owner) {
  setObjectName("semanticController");
  dispatcher=std::shared_ptr<QObject>(new QObject,[](QObject *object){object->deleteLater();});
  service=new TargetService(window);
  dock=new QDockWidget(tr("Model tree"),window);dock->setObjectName("modelTreeDock");
  auto *body=new QWidget(dock);auto *layout=new QVBoxLayout(body);
  status=new QLabel(body);status->setObjectName("semanticTreeStatus");status->setWordWrap(true);layout->addWidget(status);
  treeModel=new ModelTreeModel(this);tree=new QTreeView(body);tree->setObjectName("semanticModelTree");tree->setModel(treeModel);
  tree->setSelectionBehavior(QAbstractItemView::SelectRows);tree->header()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
  tree->setContextMenuPolicy(Qt::CustomContextMenu);layout->addWidget(tree);dock->setWidget(body);
  window->addDockWidget(Qt::RightDockWidgetArea,dock);window->menuWindow->addAction(dock->toggleViewAction());dock->hide();
  connect(treeModel,&ModelTreeModel::eyeChanged,this,[this](QString id,bool visible){setHidden(id.toStdString(),!visible);});
  connect(tree->selectionModel(),&QItemSelectionModel::currentChanged,this,[this](const QModelIndex& index){
    if(refreshing || !current() || index.data(ModelTreeModel::ResultRole).toString()!=sourceStamp.resultId)return;
    const auto role=static_cast<TreeRole>(index.data(ModelTreeModel::KindRole).toInt());
    if(role==TreeRole::Instance || role==TreeRole::Definition)select({role==TreeRole::Instance?TargetRole::Instance:TargetRole::Definition,index.data(ModelTreeModel::IdRole).toString().toStdString()});
  });
  connect(tree,&QTreeView::customContextMenuRequested,this,[this](QPoint point){
    const auto index=tree->indexAt(point);if(index.isValid())tree->setCurrentIndex(index);
    QMenu menu(tree);const auto key=selectedTarget();
    auto action=[&](QString label,bool enabled,std::function<void()> run){auto *entry=menu.addAction(label);entry->setEnabled(enabled);connect(entry,&QAction::triggered,this,std::move(run));};
    action(tr("Solo selected instance"),prepared() && key && key->role==TargetRole::Instance,[this,key]{setSolo(key->id);});
    action(tr("Clear solo"),prepared() && visibility().solo.has_value(),[this]{setSolo({});});
    action(tr("Go to call"),current() && key && key->role==TargetRole::Instance,[this]{navigate(false);});
    action(tr("Go to definition"),current() && key.has_value(),[this]{navigate(true);});
    auto target=key?service->resolve(*key):PreparedTarget{};const bool resolved=target.state==Design::ResolutionState::Resolved;
    action(tr("Fit target"),resolved && target.worldBounds && !target.worldBounds->isEmpty(),[this]{fit();});
    action(tr("Export target…"),resolved && target.localGeometry && !target.localGeometry->isEmpty(),[this]{exportSelected();});
    action(tr("Create Section"),resolved && target.worldBounds && !target.worldBounds->isEmpty() && rigidFrame(target.localToWorld),[this]{createSection();});
    action(tr("Retry target preparation"),current() && key.has_value(),[this,key]{service->retry(*key);});
    action(tr("Cancel target jobs"),current(),[this]{service->cancel();});
    menu.exec(tree->viewport()->mapToGlobal(point));
  });
  provider=window->inspectionPresentation->registerMeshProvider("semantic-parts",100,this,[this](auto captured,Selection::Stamp stamp,auto done){
    const auto generation=++preparedJobs;auto stopped=std::make_shared<std::atomic<bool>>(false);auto delivery=dispatcher;QPointer<Controller> self(this);
    (void)QtConcurrent::run([captured,stamp,done=std::move(done),generation,stopped,delivery,self]()mutable{
      auto output=prepareMeshes(*captured->semantic,stamp.resultId.toStdString(),generation,[stopped]{return stopped->load();});
      QMetaObject::invokeMethod(delivery.get(),[self,captured,stamp,done=std::move(done),output=std::move(output),stopped]()mutable{
        if(stopped->load()){done({InspectionPresentationController::Status::Cancelled,{},QStringLiteral("Semantic inspection cancelled")});return;}
        if(self && self->current() && self->scene==captured && self->sourceStamp.resultId==stamp.resultId)self->drawPaths=output.paths;
        done({output.cancelled?InspectionPresentationController::Status::Cancelled:!output.error.empty()?InspectionPresentationController::Status::Unsupported:output.meshes && output.meshes->pieces.empty()?InspectionPresentationController::Status::Empty:InspectionPresentationController::Status::Ready,
          output.meshes,QString::fromStdString(output.error)});
      },Qt::QueuedConnection);
    });
    return [stopped]{stopped->store(true);};
  });provider.setEnabled(false);
  connect(window->evaluationBinding,&Selection::EvaluationBinding::installed,this,[this]{QTimer::singleShot(0,this,&Controller::refresh);});
  connect(window->evaluationBinding,&Selection::EvaluationBinding::invalidated,this,[this]{QTimer::singleShot(0,this,&Controller::refresh);});
  connect(window->inspectionPresentation,&InspectionPresentationController::meshesChanged,this,&Controller::applyMask);
  connect(window->inspectionPresentation,&InspectionPresentationController::statusChanged,this,[this]{
    if(current() && scene->semantic->marked)status->setText(window->inspectionPresentation->reason());
    treeModel->setVisibility(visibility(),prepared());
  });
  connect(window->selectionService,&Selection::SelectionService::selectionChanged,this,&Controller::picked);
  connect(service,&TargetService::changed,this,[this]{if(window){window->qglview->update();window->inspectorDock->refresh();QTimer::singleShot(0,this,&Controller::refreshParameters);}});
  connect(service,&TargetService::exportFinished,this,[this](bool success,QString message){if(window)window->statusBar()->showMessage(success?tr("Exported %1").arg(message):message,10000);});
  inspector=window->inspectorDock->registerContribution({"semantic-model",20,this,[this](const auto&,const auto&){return buildInspector();}});
  overlay=window->viewportOverlays->registerOverlay(this,Selection::OverlayLayer::WorldOverlay,20,[this](const auto&){drawHighlight();});
  connect(window->tabManager,&TabManager::editorAboutToClose,this,[this](EditorInterface *editor){documents.erase(editor->documentId());releaseCustomizer(editor->documentId());});
  if(window->activeEditor)window->parameterDock->setWidget(customizerWidget(window->activeEditor));
  refresh();
}
Controller::~Controller(){inspector.reset();overlay.reset();request.reset();provider.reset();delete service;while(!customizers.empty())releaseCustomizer(customizers.begin()->first);delete dock;}
QWidget *Controller::customizerWidget(EditorInterface *editor){
  if(!editor)return nullptr;auto found=customizers.find(editor->documentId());if(found!=customizers.end())return found->second.tabs;
  auto *tabs=new QTabWidget(editor);tabs->setObjectName("semanticCustomizerTabs");
  tabs->addTab(editor->parameterWidget,tr("Global"));auto *panel=new ParameterPanel(window,this,tabs);tabs->addTab(panel,tr("Model"));
  customizers.emplace(editor->documentId(),CustomizerHost{editor,tabs,panel});panel->refresh();return tabs;
}
ParameterPanel *Controller::parameterPanel() const {if(!window || !window->activeEditor)return nullptr;auto found=customizers.find(window->activeEditor->documentId());return found==customizers.end()?nullptr:found->second.panel.data();}
void Controller::releaseCustomizer(const QString& id){
  auto found=customizers.find(id);if(found==customizers.end())return;auto host=found->second;customizers.erase(found);
  if(host.editor && host.tabs){host.tabs->removeTab(0);host.editor->parameterWidget->setParent(host.editor);if(window && window->parameterDock->widget()==host.tabs)window->parameterDock->setWidget(host.editor->parameterWidget);}
  delete host.tabs;
}
void Controller::refreshParameters(){if(auto *panel=parameterPanel())panel->refresh();}
bool Controller::current() const {return window && scene && window->evaluationBinding->validate(window->evaluationBinding->stamp()) && service->validate(sourceStamp);}
bool Controller::prepared() const {return current() && window->inspectionPresentation->meshes() && window->inspectionPresentation->meshes()->resultId==sourceStamp.resultId.toStdString() &&
  (window->inspectionPresentation->status()==InspectionPresentationController::Status::Ready || window->inspectionPresentation->status()==InspectionPresentationController::Status::Empty);}
Visibility Controller::visibility() const {auto found=documents.find(sourceStamp.documentId);return found==documents.end()?Visibility{}:found->second.visibility;}
std::optional<TargetKey> Controller::selectedTarget() const {auto found=documents.find(sourceStamp.documentId);return current() && found!=documents.end()?found->second.selected:std::nullopt;}
std::optional<std::string> Controller::pathForDraw(uint64_t draw) const {if(!prepared())return {};auto found=drawPaths.find(draw);return found==drawPaths.end() || found->second.empty()?std::nullopt:std::optional<std::string>(found->second);}
void Controller::refresh(){
  if(refreshing)return;QScopedValueRollback<bool> guard(refreshing,true);
  auto stamp=window->evaluationBinding->stamp();auto retained=window->evaluationBinding->scene(stamp);
  if(retained && retained==scene && service->validate(sourceStamp)){applyMask();refreshParameters();return;}
  scene.reset();drawPaths.clear();lastMask=lastGeneration=0;
  if(!retained || !retained->semantic){treeModel->setScene({},stamp.resultId,window->evaluationBinding->staleReason());status->setText(tr("Model inspection is stale or unavailable"));refreshParameters();return;}
  scene=retained;sourceStamp=stamp;auto& state=documents[stamp.documentId];state.visibility.prune(*scene->semantic);
  if(state.selected && (state.selected->role==TargetRole::Instance?!scene->semantic->instances.count(state.selected->id):!scene->semantic->definitions.count(state.selected->id)))state.selected.reset();
  treeModel->setScene(scene->semantic,stamp.resultId);tree->expandAll();
  if(scene->semantic->marked && scene->semantic->valid()){
    provider.setEnabled(true);if(!request)request=window->inspectionPresentation->requestMeshes("semantic-model-tree",this);
    if(!state.shown){dock->show();state.shown=true;}
  }else {provider.setEnabled(false);request.reset();}
  status->setText(scene->semantic->valid()?QString():tr("Semantic declarations are invalid; inspect source diagnostics"));
  if(state.selected){auto index=treeModel->find(state.selected->role==TargetRole::Instance?TreeRole::Instance:TreeRole::Definition,state.selected->id);tree->setCurrentIndex(index);service->resolve(*state.selected);}
  window->inspectorDock->refresh();
  refreshParameters();
}
bool Controller::setHidden(const std::string& path,bool hidden){
  if(!prepared() || (!path.empty() && !scene->semantic->instances.count(path)))return false;
  auto& state=documents[sourceStamp.documentId].visibility;
  if(path.empty())state.otherVisible=!hidden;else if(hidden)state.hidden.insert(path);else state.hidden.erase(path);
  ++maskSerial;applyMask();return true;
}
bool Controller::setSolo(std::optional<std::string> path){
  if(!prepared() || (path && !scene->semantic->instances.count(*path)))return false;
  documents[sourceStamp.documentId].visibility.solo=std::move(path);++maskSerial;applyMask();return true;
}
void Controller::applyMask(){
  if(applying || !prepared() || !request)return;QScopedValueRollback<bool> guard(applying,true);
  const auto meshes=window->inspectionPresentation->meshes();const auto state=visibility();
  if(lastMask!=maskSerial+1 || lastGeneration!=meshes->geometryGeneration){
    InspectionMask mask;mask.serial=maskSerial;for(const auto& [id,path]:drawPaths)if(path.empty()?state.otherVisible && !state.solo:state.visible(path))mask.visible.insert(id);
    auto stamp=window->evaluationBinding->stamp();if(request.setMask(mask,stamp,meshes->geometryGeneration)){lastMask=maskSerial+1;lastGeneration=meshes->geometryGeneration;}
    treeModel->setVisibility(state,true);
  }
  rebind();window->qglview->update();
}
bool Controller::select(TargetKey key){
  if(!current() || (key.role==TargetRole::Instance?!scene->semantic->instances.count(key.id):!scene->semantic->definitions.count(key.id)))return false;
  documents[sourceStamp.documentId].selected=key;service->resolve(key);
  {QScopedValueRollback<bool> guard(refreshing,true);tree->setCurrentIndex(treeModel->find(key.role==TargetRole::Instance?TreeRole::Instance:TreeRole::Definition,key.id));}
  rebind();window->inspectorDock->refresh();window->qglview->update();refreshParameters();
  if(window->activeEditor){auto host=customizers.find(window->activeEditor->documentId());if(host!=customizers.end() && host->second.tabs)host->second.tabs->setCurrentIndex(1);}
  emit targetChanged();return true;
}
void Controller::rebind(){
  if(rebinding || !prepared())return;const auto key=selectedTarget();if(!key || key->role!=TargetRole::Instance)return;
  QScopedValueRollback<bool> guard(rebinding,true);const auto state=visibility();
  for(const auto& piece:window->inspectionPresentation->meshes()->pieces){
    const auto owned=drawPaths.find(piece.drawId);
    if(owned==drawPaths.end() || owned->second!=key->id || !state.visible(key->id) || piece.role==InspectionPiece::Role::Background)continue;
    const Selection::ObjectKey object{sourceStamp.resultId,piece.drawId,piece.nodeIndex};
    if(window->selectionService->state().object!=std::optional<Selection::ObjectKey>(object))window->selectionService->selectObject(object,window->evaluationBinding->stamp());return;
  }
  if(window->selectionService->state().object)window->selectionService->clear();
}
void Controller::picked(const Selection::State& state){
  if(rebinding || !current() || !state.object)return;
  if(auto path=pathForDraw(state.object->drawId))select({TargetRole::Instance,*path});
  else {documents[sourceStamp.documentId].selected.reset();refreshParameters();window->inspectorDock->refresh();window->qglview->update();emit targetChanged();}
}
void Controller::navigate(bool definition){
  const auto key=selectedTarget();if(!key || !scene->semantic->syntax)return;
  std::optional<SourceProvenance::Site> site;
  if(key->role==TargetRole::Definition)site=scene->semantic->definitions.at(key->id).syntax;
  else {const auto& instance=scene->semantic->instances.at(key->id);site=definition?scene->semantic->definitions.at(instance.definitionId).syntax:instance.call;}
  QString detail;if(!site || !SourceProvenance::ProvenanceService::forWindow(window)->navigateSite(*scene->semantic->syntax,*site,window->evaluationBinding->stamp(),&detail))window->statusBar()->showMessage(detail.isEmpty()?tr("Source range is unavailable"):detail,10000);
}
void Controller::fit(){const auto key=selectedTarget();if(!key)return;const auto target=service->resolve(*key);if(target.state!=Design::ResolutionState::Resolved || !target.worldBounds || target.worldBounds->isEmpty())return;
  Camera camera=window->qglview->cam;camera.autocenter=true;camera.viewAll(*target.worldBounds);window->qglview->navigation()->apply(CameraPose::from(camera));}
void Controller::createSection(){
  const auto key=selectedTarget();if(!key)return;const auto target=service->resolve(*key);
  if(target.state!=Design::ResolutionState::Resolved || !target.worldBounds || target.worldBounds->isEmpty() || !rigidFrame(target.localToWorld))return;
  auto vector=[](Vector3d v){VectorType values(nullptr);for(int i=0;i<3;++i)values.emplace_back(v[i]);return Value(std::move(values));};
  ObjectType value(nullptr);value.set("id",Value("section-"+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()));
  value.set("target",Design::ownedValue(targetToken(*key).data));value.set("frame",Design::ownedValue(targetToken(*key).data));
  Vector3d origin=Vector3d::Zero();if(target.localBounds && !target.localBounds->isEmpty())origin=target.localBounds->center();
  value.set("origin",vector(origin));value.set("normal",vector(Vector3d::UnitZ()));
  std::string error;auto document=Design::DesignDocument::forEditor(*window->activeEditor,*window);
  if(!document->createTransient("Section",Value(std::move(value)),&error))window->statusBar()->showMessage(QString::fromStdString(error),10000);
}
void Controller::exportSelected(){
  const auto key=selectedTarget();if(!key)return;const auto target=service->resolve(*key);if(target.state!=Design::ResolutionState::Resolved || !target.localGeometry || target.localGeometry->isEmpty())return;
  QDialog dialog(window);dialog.setWindowTitle(tr("Export semantic target"));auto *layout=new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(tr("Target: %1\nCaptured document configuration: %2\nExport uses the evaluated parameters and $preview setting.").arg(QString::fromStdString(key->id),sourceStamp.configurationFingerprint),&dialog));
  auto *space=new QComboBox(&dialog);space->addItem(tr("Local"));if(key->role==TargetRole::Instance)space->addItem(tr("As placed"));layout->addWidget(space);
  auto *format=new QComboBox(&dialog);auto formats=target.localGeometry->getDimension()==3?fileformat::all3D():fileformat::all2D();
  formats.erase(std::remove_if(formats.begin(),formats.end(),[](auto f){return f==FileFormat::NEFDBG || f==FileFormat::NEF3 || f==FileFormat::POV;}),formats.end());
  for(const auto f:formats)format->addItem(QString::fromStdString(fileformat::info(f).description),static_cast<int>(f));layout->addWidget(format);
  auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  if(dialog.exec()!=QDialog::Accepted)return;const auto selectedFormat=static_cast<FileFormat>(format->currentData().toInt());const auto info=fileformat::info(selectedFormat);
  auto exportInfo=createExportInfo(selectedFormat,info,scene->documentPath,&window->qglview->cam,{});
  if(!configureGeometryExportOptions(selectedFormat,exportInfo))return;
  const auto path=QFileDialog::getSaveFileName(window,tr("Export target"),QString(),QString::fromStdString(info.description+" (*."+info.suffix+")"));if(path.isEmpty())return;
  service->exportTarget(*key,space->currentIndex()==0?ExportSpace::Local:ExportSpace::AsPlaced,path,std::move(exportInfo));
}
QWidget *Controller::buildInspector(){
  auto *body=new QWidget;body->setObjectName("semanticInspector");auto *layout=new QVBoxLayout(body);layout->setContentsMargins(0,0,0,0);
  const auto key=selectedTarget();if(!key){layout->addWidget(new QLabel(current()?tr("Semantic target: select a model tree row or a mapped Part surface."):tr("Semantic target is stale or unavailable."),body));return body;}
  auto target=service->resolve(*key);QString text=tr("Semantic %1: %2\n%3").arg(key->role==TargetRole::Instance?tr("instance"):tr("definition defaults"),QString::fromStdString(key->id),QString::fromStdString(target.diagnostic));
  if(key->role==TargetRole::Instance){const auto& instance=scene->semantic->instances.at(key->id);text+=tr("\nDefinition: %1\nParameters:").arg(QString::fromStdString(instance.definitionId));for(const auto& [name,value]:instance.parameters)text+=QString("\n%1 = %2").arg(QString::fromStdString(name),QString::fromStdString(value.displayedValue));
    auto matrix=[&text](QString title,const Transform3d& frame){text+="\n"+title;for(int row=0;row<3;++row){text+="\n";for(int column=0;column<4;++column){if(column)text+="  ";text+=QString::number(frame.matrix()(row,column),'g',9);}}};
    matrix(tr("Local to parent (affine rows)"),instance.localToParent);matrix(tr("Local to model (affine rows)"),instance.localToWorld);
  }
  else for(const auto& [name,value]:target.parameters)text+=QString("\n%1 = %2").arg(QString::fromStdString(name),QString::fromStdString(value.displayedValue));
  auto *label=new QLabel(text,body);label->setObjectName("semanticInspectorStatus");label->setWordWrap(true);label->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(label);
  auto button=[&](QString label,const char *name,bool enabled,std::function<void()> run){auto *value=new QPushButton(label,body);value->setObjectName(name);value->setEnabled(enabled);layout->addWidget(value);connect(value,&QPushButton::clicked,this,std::move(run));};
  const bool resolved=target.state==Design::ResolutionState::Resolved;
  button(tr("Go to call"),"semanticGoToCall",key->role==TargetRole::Instance,[this]{navigate(false);});button(tr("Go to definition"),"semanticGoToDefinition",true,[this]{navigate(true);});
  button(tr("Fit target"),"semanticFitTarget",resolved && target.worldBounds && !target.worldBounds->isEmpty(),[this]{fit();});
  button(tr("Export target…"),"semanticExportTarget",resolved && target.localGeometry && !target.localGeometry->isEmpty(),[this]{exportSelected();});
  button(tr("Create Section"),"semanticCreateSection",resolved && target.worldBounds && !target.worldBounds->isEmpty() && rigidFrame(target.localToWorld),[this]{createSection();});
  button(tr("Retry target preparation"),"semanticRetryTarget",!resolved,[this,key]{service->retry(*key);});
  return body;
}
void Controller::drawHighlight(){
  const auto key=selectedTarget();if(!key || !prepared())return;const auto target=service->resolve(*key);if(target.state!=Design::ResolutionState::Resolved || !target.worldBounds || target.worldBounds->isEmpty())return;
  if(key->role==TargetRole::Instance && !visibility().visible(key->id) && !visibility().solo)return;
  glColor4f(1.0f,0.8f,0.1f,1.0f);glLineWidth(2.0f);glBegin(GL_LINES);
  for(int corner=0;corner<8;++corner)for(int bit=0;bit<3;++bit)if(!(corner&(1<<bit))){const Vector3d a=target.worldBounds->corner(static_cast<BoundingBox::CornerType>(corner)),b=target.worldBounds->corner(static_cast<BoundingBox::CornerType>(corner|(1<<bit)));glVertex3dv(a.data());glVertex3dv(b.data());}glEnd();
}
}
