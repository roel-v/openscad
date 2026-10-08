// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/insertion/InsertionController.h"
#include "core/insertion/InsertionPreviewProtocol.h"
#include "core/providers/ProviderRegistry.h"
#include "core/providers/LibraryResolution.h"
#include "core/providers/ContentDigest.h"
#include "core/parsersettings.h"
#include "core/SourceFile.h"
#include "gui/MainWindow.h"
#include "gui/TabManager.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/selection/ProvenanceService.h"
#include "gui/semantic/SemanticController.h"
#include "gui/source/SourceEditTransaction.h"
#include "glview/ScopedGLState.h"
#include "glview/SurfacePickMath.h"
#include "platform/PlatformUtils.h"
#include <QApplication>
#include <QDockWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStandardItemModel>
#include <QStatusBar>
#include <cmath>
#include <utility>
namespace Insertion {
namespace {
QString qs(const std::string& s){return QString::fromUtf8(s.data(),qsizetype(s.size()));}
std::string sourceKey(EditorInterface *editor){return editor->filepath.isEmpty()?"session:"+editor->documentId().toStdString():EvaluationInputs::fileKey(std::filesystem::u8path(editor->filepath.toUtf8().toStdString()));}
bool under(const std::string& path,const std::string& root){const auto a=qs(path),b=qs(root);return a.compare(b,Qt::CaseInsensitive)==0 || a.startsWith(b+"/",Qt::CaseInsensitive);}
std::string directory(const SourceTarget& target,const Selection::EvaluatedScene& scene){return target.body.fileKey.rfind("session:",0)==0?EvaluationInputs::fileKey(scene.backing->parsedRoot->modulePath()):EvaluationInputs::fileKey(std::filesystem::u8path(target.body.fileKey).parent_path());}
}
Controller::Controller(MainWindow *mainWindow):QObject(mainWindow),window(mainWindow){
  setObjectName("insertionController");dock=new QDockWidget(tr("Add component or geometry"),window);dock->setObjectName("insertionDock");
  auto body=new QWidget(dock);auto layout=new QVBoxLayout(body);search=new QLineEdit(body);search->setObjectName("insertionSearch");search->setPlaceholderText(tr("Search items"));layout->addWidget(search);
  palette=new QListWidget(body);palette->setObjectName("insertionPalette");palette->setMaximumHeight(180);layout->addWidget(palette);
  destinations=new QComboBox(body);destinations->setObjectName("insertionDestination");layout->addWidget(destinations);auto openSourceButton=new QPushButton(tr("Open source and restart insertion"),body);openSourceButton->setObjectName("insertionOpenSource");layout->addWidget(openSourceButton);connect(openSourceButton,&QPushButton::clicked,this,[this]{openSource(size_t(std::max(0,destinations->currentIndex())));});
  scope=new QLabel(body);scope->setObjectName("insertionScope");scope->setWordWrap(true);layout->addWidget(scope);
  shared=new QCheckBox(tr("Edit shared definition in every listed instance"),body);shared->setObjectName("insertionSharedScope");layout->addWidget(shared);
  auto scroll=new QScrollArea(body);scroll->setWidgetResizable(true);parameters=new QWidget(scroll);parameters->setObjectName("insertionParameters");fields=new QFormLayout(parameters);scroll->setWidget(parameters);layout->addWidget(scroll);
  auto placementForm=new QFormLayout;for(int axis=0;axis<3;++axis){offsets[axis]=new QDoubleSpinBox(body);offsets[axis]->setObjectName(QString("insertionOffset%1").arg(axis));offsets[axis]->setDecimals(10);offsets[axis]->setRange(-1e100,1e100);placementForm->addRow(axis==0?tr("Plane X"):axis==1?tr("Plane Y"):tr("Normal offset"),offsets[axis]);connect(offsets[axis],qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{if(!refreshing && placement)setPlacement({offsets[0]->value(),offsets[1]->value(),offsets[2]->value()},spin->value(),flip->isChecked());});}
  spin=new QDoubleSpinBox(body);spin->setObjectName("insertionSpin");spin->setDecimals(10);spin->setRange(-1e9,1e9);placementForm->addRow(tr("Spin (degrees)"),spin);layout->addLayout(placementForm);
  flip=new QCheckBox(tr("Flip to Align"),body);flip->setObjectName("insertionFlip");layout->addWidget(flip);
  anchors=new QComboBox(body);anchors->setObjectName("insertionAnchor");layout->addWidget(anchors);
  auto move=new QPushButton(tr("Move in plane (G)"),body);move->setObjectName("insertionMove");layout->addWidget(move);connect(move,&QPushButton::clicked,this,[this]{armDrag(Drag::Move);window->qglview->setFocus();});
  auto rotate=new QPushButton(tr("Spin (R)"),body);rotate->setObjectName("insertionRotate");layout->addWidget(rotate);connect(rotate,&QPushButton::clicked,this,[this]{armDrag(Drag::Spin);window->qglview->setFocus();});
  status=new QLabel(body);status->setObjectName("insertionStatus");status->setWordWrap(true);layout->addWidget(status);
  commitButton=new QPushButton(tr("Commit"),body);commitButton->setObjectName("insertionCommit");layout->addWidget(commitButton);connect(commitButton,&QPushButton::clicked,this,&Controller::commit);
  auto cancelButton=new QPushButton(tr("Cancel (Escape)"),body);cancelButton->setObjectName("insertionCancel");layout->addWidget(cancelButton);connect(cancelButton,&QPushButton::clicked,this,&Controller::cancel);
  auto reload=new QPushButton(tr("Reload providers"),body);reload->setObjectName("insertionReloadProviders");layout->addWidget(reload);connect(reload,&QPushButton::clicked,this,&Controller::reloadProviders);
  auto panelScroll=new QScrollArea(dock);panelScroll->setWidgetResizable(true);panelScroll->setWidget(body);dock->setWidget(panelScroll);window->addDockWidget(Qt::RightDockWidgetArea,dock);dock->hide();window->menuWindow->addAction(dock->toggleViewAction());
  connect(dock,&QDockWidget::visibilityChanged,this,[this](bool visible){if(!visible && !closing && currentState!=State::Idle)cancel();});
  connect(search,&QLineEdit::textChanged,this,[this]{refreshPalette();});connect(palette,&QListWidget::itemClicked,this,[this](auto item){choose(item->data(Qt::UserRole).toString().toStdString());});
  connect(destinations,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){if(!refreshing && index>=0)selectTarget(size_t(index));});
  connect(shared,&QCheckBox::toggled,this,[this](bool yes){if(!refreshing)approveShared(yes);});
  connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{if(!refreshing && placement)setPlacement(placement->offset,spin->value(),flip->isChecked());});
  connect(flip,&QCheckBox::toggled,this,[this](bool yes){if(!refreshing && placement)setPlacement(placement->offset,spin->value(),yes);});
  connect(anchors,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){if(!refreshing && index>=0)setNamedAnchor(index==0?std::nullopt:std::optional<Anchors::ReferenceKey>(anchorKeys.at(size_t(index-1))));});
  auto action=[&](const char *id,QString label,QKeySequence key,auto callback){auto value=new QAction(label,this);value->setObjectName(id);value->setShortcut(key);value->setShortcutContext(Qt::WidgetWithChildrenShortcut);window->qglview->addAction(value);window->menu_Design->addAction(value);connect(value,&QAction::triggered,this,callback);};
  action("insertion.add",tr("Add…"),QKeySequence("Shift+A"),[this]{add();});action("insertion.move",tr("Move insertion"),QKeySequence("G"),[this]{armDrag(Drag::Move);});action("insertion.spin",tr("Spin insertion"),QKeySequence("R"),[this]{armDrag(Drag::Spin);});action("insertion.flip",tr("Flip insertion"),QKeySequence("F"),[this]{if(placement)setPlacement(placement->offset,placement->spinDegrees,!placement->flipped);});action("insertion.commit",tr("Commit insertion"),QKeySequence(Qt::Key_Return),[this]{commit();});action("insertion.cancel",tr("Cancel insertion"),QKeySequence(Qt::Key_Escape),[this]{cancel();});
  window->qglview->installEventFilter(this);qApp->installEventFilter(this);
  debounce.setSingleShot(true);debounce.setInterval(150);connect(&debounce,&QTimer::timeout,this,&Controller::launch);
  watch.setInterval(250);connect(&watch,&QTimer::timeout,this,[this]{if(currentState!=State::Idle && currentState!=State::Picking && currentState!=State::Committing && !current()){cancel();window->statusBar()->showMessage(tr("Insertion cancelled: source, configuration or provider input changed"),10000);}});
  elapsed.setInterval(1000);connect(&elapsed,&QTimer::timeout,this,[this]{if(process && process->running())status->setText(statusText+tr(" (%1 s; Cancel available)").arg(process->elapsedMilliseconds()/1000));});
  connect(window->evaluationBinding,&Selection::EvaluationBinding::invalidated,this,[this]{if(currentState!=State::Idle && currentState!=State::Preparing && currentState!=State::Committing && !this->window->evaluationBinding->validateModel(stamp))cancel();});
  connect(window->tabManager,&TabManager::currentEditorChanged,this,[this](auto){if(currentState==State::Preparing)cancel();});
  connect(window->evaluationBinding,&Selection::EvaluationBinding::evaluationFinished,this,[this](EditorInterface *editor){
    if(currentState!=State::Preparing || rootEditor!=editor)return;
    const auto ticket=serial;
    QTimer::singleShot(0,this,[this,ticket]{
      if(ticket!=serial || currentState!=State::Preparing)return;
      if(!rootEditor || rootEditor!=window->activeEditor || rootEditor->sourceRevision!=preparingRevision || !window->evaluationBinding->inspectionReady()) {
        cancel();window->statusBar()->showMessage(tr("Insertion preparation did not complete. Check evaluation messages, then try Add again."),10000);return;
      }
      setState(State::Idle,{});add(); // The old hit was invalidated; pick against the new result.
    });
  });
  connect(window->inspectionPresentation,&InspectionPresentationController::draftDisplayChanged,this,[this](bool active,bool){if(!active && display && !closing && currentState!=State::Idle && currentState!=State::Committing)cancel();});
  setState(State::Idle,{});
}
Controller::~Controller(){cancel();delete dock;}
void Controller::setState(State state,QString message){currentState=state;statusText=std::move(message);status->setText(statusText);commitButton->setEnabled(state==State::Ready);
  for(const char *name:{"insertion.move","insertion.spin","insertion.flip","insertion.commit","insertion.cancel"})if(auto action=findChild<QAction *>(name))action->setEnabled(std::string(name)=="insertion.cancel"?state!=State::Idle:(std::string(name)=="insertion.commit"?state==State::Ready:placement.has_value() && state!=State::Committing));emit changed();}
void Controller::stopProcess(){if(process){process->cancel();process->deleteLater();process.clear();}elapsed.stop();}
void Controller::cancel(){if(closing)return;QScopedValueRollback<bool> guard(closing,true);if(window && window->evaluationBinding)window->evaluationBinding->releaseInspection(this);debounce.stop();watch.stop();stopProcess();++serial;drag=Drag::None;if(window && window->viewportTools)window->viewportTools->finish(std::exchange(tool,0));overlay.reset();display.reset();plan.reset();result.reset();ghost.reset();placement.reset();fragment.reset();binding.reset();target.reset();targetOptions.clear();inventory.reset();capturedInputs.reset();scene.reset();hit.reset();rootEditor.clear();targetEditor.clear();controls.clear();setState(State::Idle,{});dock->hide();if(window && window->qglview)window->qglview->update();}
bool Controller::add(){
  if(currentState!=State::Idle){cancel();return false;}
  if(!window->activeEditor || GuiLocker::isLocked())return false;
  rootEditor=window->activeEditor;
  window->evaluationBinding->requestInspection(this,rootEditor);
  if(!window->evaluationBinding->inspectionReady()) {
    preparingRevision=rootEditor->sourceRevision;const auto ticket=++serial;
    setState(State::Preparing,tr("Preparing source information for insertion…"));dock->show();dock->raise();
    QTimer::singleShot(0,this,[this,ticket]{
      if(ticket!=serial || currentState!=State::Preparing)return;
      if(!rootEditor || rootEditor!=window->activeEditor || rootEditor->sourceRevision!=preparingRevision || GuiLocker::isLocked()){cancel();return;}
      window->actionRenderPreview();
    });
    return true;
  }
  const auto selected=window->selectionService->state().surface;
  if(selected && window->evaluationBinding->validate(selected->stamp))return begin(*selected);
  stamp=window->evaluationBinding->stamp();setState(State::Picking,tr("Pick a source-owned surface; Escape cancels"));
  tool=window->viewportTools->beginPick({"Insertion",stamp,true},this,[this](auto result){tool=0;if(result.hit)begin(*result.hit);else cancel();});
  if(!tool)cancel();
  return tool!=0;
}
bool Controller::begin(const Selection::Hit& picked){cancel();if(!window->evaluationBinding->validate(picked.stamp) || !picked.object.nodeIndex)return false;
  if(!window->evaluationBinding->inspectionReady())return add();
  window->evaluationBinding->requestInspection(this,window->activeEditor);
  try {stamp=picked.stamp;scene=window->evaluationBinding->scene(stamp);if(!scene || !scene->backing)throw ProtocolError("Evaluated source is unavailable");rootEditor=window->activeEditor;hit=picked;cameraUp=CameraPose::matrix(window->qglview->cam.object_rot).transpose()*Vector3d::UnitZ();configuration=captureConfiguration(*scene->backing->builtin,rootEditor->parameterWidget->evaluationValues());auto captured=SourceInventory::capture(scene->backing->parsedRoot,scene->backing->libraries,scene->inputs,scene->root,scene->provenance,scene->semantic,configuration.defines);if(!captured)throw ProtocolError(captured.diagnostic->message);inventory=std::move(*captured.value);
    const auto rootDirectory=EvaluationInputs::fileKey(scene->backing->parsedRoot->modulePath());auto resources=PlatformUtils::resourcePath("providers");discovered=Providers::discoverProviders({resources.empty()?"":EvaluationInputs::fileKey(resources),PlatformUtils::userConfigPath(),rootDirectory,{}});registry=Providers::ProviderRegistry::load(discovered.catalogs);auto live=Selection::EvaluationBinding::capture(*window,*rootEditor);capturedInputs=std::make_shared<EvaluationInputs>(live->root(),live->capturedEditors(),EvaluationInputs::ReadPolicy::CaptureMissing,scene->inputs->consumed());bindingContext=context(rootDirectory,capturedInputs);
    // Seeds retain versions; explicitly consume the whole displayed closure before constructing a worker request.
    for(const auto& [key,input]:scene->inputs->consumed()){
      if(key==capturedInputs->root().fileKey)continue;
      const auto& retained=capturedInputs->read(std::filesystem::u8path(key));
      if(retained.fingerprint!=input.fingerprint || retained.editorId!=input.editorId || retained.editorRevision!=input.editorRevision)throw ProtocolError("Displayed input changed during insertion capture");
    }
    setState(State::Choosing,tr("Choose an item, then its editable destination"));refreshPalette();dock->show();dock->raise();search->setFocus();tool=window->viewportTools->claimExternal("Insertion",this,[this](auto){tool=0;if(currentState!=State::Committing)cancel();});if(!tool || !window->viewportTools->setFocusGroup(tool,{window->qglview,dock}))throw ProtocolError("Could not acquire insertion input session");watch.start();return true;
  }catch(const std::exception& error){auto message=QString::fromUtf8(error.what());cancel();window->statusBar()->showMessage(message,10000);return false;}
}
Providers::LibraryResolutionContext Controller::context(const std::string& sourceDirectory,std::shared_ptr<EvaluationInputs> inputs)const{
  auto resolved=Providers::nativeResolutionContext(sourceDirectory,discovered.libraryBindings);if(!resolved)throw ProtocolError(resolved.diagnostic->message);auto result=*resolved.value;
  result.readBytes=[inputs](const std::string& key)->Providers::Result<Providers::SourceBytes>{try{const auto& source=inputs->read(std::filesystem::u8path(key));return {Providers::SourceBytes{source.utf8,source.fingerprint+":"+source.editorId.value_or("")+":"+(source.editorRevision?std::to_string(*source.editorRevision):"")},{}};}catch(const std::exception& error){return {{},Providers::Diagnostic{"MissingLibrary",key,"",error.what()}};}};return result;
}
std::map<std::string,EditableDocument> Controller::documents()const{std::map<std::string,EditableDocument> out;for(auto editor:window->tabManager->editorList){auto key=sourceKey(editor);bool owned=key==scene->inputs->root().fileKey;if(!owned){owned=true;for(const auto& path:configuration.libraryPaths)if(under(key,EvaluationInputs::fileKey(std::filesystem::u8path(path))))owned=false;}out[key]={true,!editor->isSourceReadOnly(),owned};}return out;}
void Controller::refreshPalette(){QSignalBlocker blocker(palette);palette->clear();if(!registry || !capturedInputs)return;const auto filter=search->text();for(const auto& item:Providers::ProviderRegistry::query(registry,bindingContext)){const auto& definition=registry->items.at(item.qualifiedId);QString text=qs(item.label);for(const auto& category:definition.category)text+=" · "+qs(category);if(!text.contains(filter,Qt::CaseInsensitive) && !qs(item.qualifiedId).contains(filter,Qt::CaseInsensitive))continue;auto row=new QListWidgetItem(text,palette);row->setData(Qt::UserRole,qs(item.qualifiedId));row->setToolTip(qs(definition.description)+(item.diagnostic?"\n"+qs(item.diagnostic->message):QString()));if(!item.binding){row->setText(text+" — "+qs(Providers::availabilityName(item.availability)));row->setFlags(row->flags()&~Qt::ItemIsEnabled);}}
  for(const auto& diagnostic:discovered.diagnostics){auto row=new QListWidgetItem(qs(diagnostic.message),palette);row->setFlags(Qt::NoItemFlags);}for(const auto& diagnostic:registry->diagnostics){auto row=new QListWidgetItem(qs(diagnostic.message),palette);row->setFlags(Qt::NoItemFlags);}
}
bool Controller::choose(const std::string& id){if(!inventory || !hit || !current() || !registry->items.count(id))return false;stopProcess();display.reset();result.reset();plan.reset();fragment.reset();placement.reset();target.reset();targetEditor.clear();binding.reset();ghost.reset();
  try {auto live=Selection::EvaluationBinding::capture(*window,*rootEditor);capturedInputs=std::make_shared<EvaluationInputs>(live->root(),live->capturedEditors(),EvaluationInputs::ReadPolicy::CaptureMissing,scene->inputs->consumed());
    for(const auto& [key,input]:scene->inputs->consumed()){
      if(key==capturedInputs->root().fileKey)continue;
      const auto& retained=capturedInputs->read(std::filesystem::u8path(key));
      if(retained.fingerprint!=input.fingerprint || retained.editorId!=input.editorId || retained.editorRevision!=input.editorRevision)throw ProtocolError("Displayed input changed during insertion capture");
    }
    bindingContext=context(EvaluationInputs::fileKey(scene->backing->parsedRoot->modulePath()),capturedInputs);auto selected=Providers::ProviderRegistry::bind(registry,id,bindingContext);if(!selected)throw ProtocolError(selected.diagnostic->message);binding=std::move(*selected.value);targetOptions=inventory->targets(*hit->object.nodeIndex,registry->items.at(id).insertionRole==Providers::InsertionRole::Component,documents());
    {QScopedValueRollback<bool> guard(refreshing,true);destinations->clear();for(const auto& option:targetOptions){destinations->addItem(option.target?qs(option.target->label)+" · "+qs(option.target->body.fileKey)+tr(" [%1..%2]").arg(option.target->region.begin).arg(option.target->region.end):qs(option.diagnostic->message));if(!option.target)if(auto model=qobject_cast<QStandardItemModel *>(destinations->model()))model->item(destinations->count()-1)->setEnabled(false);}destinations->setCurrentIndex(-1);shared->setChecked(false);}
    buildParameters();setState(State::Configuring,tr("Select a proven editable source destination"));if(targetOptions.size()==1 && targetOptions[0].target)return selectTarget(0);refreshScope();return true;
  }catch(const std::exception& error){setState(State::Configuring,QString::fromUtf8(error.what()));return false;}
}
void Controller::buildParameters(){QScopedValueRollback<bool> guard(refreshing,true);while(fields->rowCount())fields->removeRow(0);controls.clear();if(!binding)return;for(const auto& spec:registry->items.at(binding->qualifiedId).parameters){QWidget *control=nullptr;if(spec.type==Providers::ParameterSpec::Type::Bool){auto box=new QCheckBox(parameters);box->setChecked(std::get<bool>(spec.defaultValue));connect(box,&QCheckBox::toggled,this,[this]{parametersChanged();});control=box;}else if(spec.type==Providers::ParameterSpec::Type::Enum){auto combo=new QComboBox(parameters);for(const auto& option:spec.enumValues)combo->addItem(qs(option));combo->setCurrentText(qs(std::get<std::string>(spec.defaultValue)));connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{parametersChanged();});control=combo;}else{auto line=new QLineEdit(parameters);std::visit([&](const auto& value){using T=std::decay_t<decltype(value)>;if constexpr(std::is_same_v<T,std::string>)line->setText(qs(value));else if constexpr(std::is_same_v<T,int64_t>)line->setText(QString::number(qlonglong(value)));else if constexpr(std::is_same_v<T,double>)line->setText(QString::number(value,'g',17));},spec.defaultValue);if(spec.type==Providers::ParameterSpec::Type::String)line->setMaxLength(int(spec.maxLength));connect(line,&QLineEdit::textChanged,this,[this]{parametersChanged();});control=line;}control->setObjectName("insertionParameter_"+qs(spec.id));controls.emplace(spec.id,control);fields->addRow(qs(spec.label)+(spec.unit.empty()?QString():" ("+qs(spec.unit)+")"),control);}}
bool Controller::selectTarget(size_t index){if(!binding || index>=targetOptions.size() || !targetOptions[index].target)return false;stopProcess();result.reset();plan.reset();display.reset();placement.reset();ghost.reset();target=*targetOptions[index].target;targetEditor.clear();const auto& input=inventory->input(target->body.fileKey);for(auto editor:window->tabManager->editorList)if(input.editorId && editor->documentId().toStdString()==*input.editorId)targetEditor=editor;if(!targetEditor)return false;
  try {bindingContext=context(directory(*target,*scene),capturedInputs);auto bound=Providers::ProviderRegistry::bind(registry,binding->qualifiedId,bindingContext);if(!bound)throw ProtocolError(bound.diagnostic->message);binding=std::move(*bound.value);{QScopedValueRollback<bool> guard(refreshing,true);destinations->setCurrentIndex(int(index));shared->setChecked(false);}parametersChanged();refreshScope();return true;}catch(const std::exception& error){setState(State::Configuring,QString::fromUtf8(error.what()));return false;}}
void Controller::refreshScope(){QScopedValueRollback<bool> guard(refreshing,true);shared->setVisible(target && target->shared);scope->setText(target?qs(target->label)+" · "+(registry->items.at(binding->qualifiedId).insertionRole==Providers::InsertionRole::Component?tr("Separate Component"):tr("Part geometry"))+" · "+(registry->items.at(binding->qualifiedId).operation==Providers::Operation::Subtractive?tr("Subtract"):tr("Add"))+"\n"+qs(registry->items.at(binding->qualifiedId).providerId)+" "+qs(registry->items.at(binding->qualifiedId).providerVersion)+[&]{QString text;for(const auto& library:binding->libraries)text+=" · "+qs(library.releaseLabel)+" / "+qs(library.profileId);return text;}()+"\n"+(target->reusableDefinition?tr("Reusable definition change. Current affected instances:\n"):QString())+[&]{QString text;for(const auto& path:target->affectedPaths)text+=qs(path)+"\n";return text;}():tr("No eligible source selected"));anchors->clear();anchorKeys.clear();anchors->addItem(tr("Fixed capture — follows placement, not surface changes"));anchors->setEnabled(fragment && fragment->insertionRole==Providers::InsertionRole::Component);if(anchors->isEnabled() && scene && scene->semantic)if(auto references=Anchors::referenceScene(*scene->semantic))for(const auto& [key,reference]:references->references)if(std::holds_alternative<Anchors::Anchor>(reference.value)){anchorKeys.push_back(key);anchors->addItem(qs(key.owner.id)+" · "+qs(reference.label.empty()?key.id:reference.label));}if(placement && placement->namedTarget){auto found=std::find(anchorKeys.begin(),anchorKeys.end(),*placement->namedTarget);if(found!=anchorKeys.end())anchors->setCurrentIndex(int(found-anchorKeys.begin()+1));}}
bool Controller::setValues(const Providers::ParameterValues& values){if(!binding)return false;
  for(const auto& [id,value]:values){auto found=controls.find(id);if(found==controls.end())return false;if(qobject_cast<QCheckBox *>(found->second) && !std::holds_alternative<bool>(value))return false;if(auto combo=qobject_cast<QComboBox *>(found->second))if(!std::holds_alternative<std::string>(value) || combo->findText(qs(std::get<std::string>(value)))<0)return false;}
  {QScopedValueRollback<bool> guard(refreshing,true);for(const auto& [id,value]:values){auto control=controls.at(id);if(auto check=qobject_cast<QCheckBox *>(control))check->setChecked(std::get<bool>(value));else if(auto combo=qobject_cast<QComboBox *>(control))combo->setCurrentText(qs(std::get<std::string>(value)));else {auto line=qobject_cast<QLineEdit *>(control);std::visit([&](const auto& v){using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::string>)line->setText(qs(v));else if constexpr(std::is_same_v<T,int64_t>)line->setText(QString::number(qlonglong(v)));else if constexpr(std::is_same_v<T,double>)line->setText(QString::number(v,'g',17));},value);}}}parametersChanged();return fragment.has_value();}
void Controller::parametersChanged(){if(refreshing || !binding)return;stopProcess();plan.reset();result.reset();fragment.reset();setState(State::Configuring,tr("Configure insertion"));try{Providers::ParameterValues values;for(const auto& spec:registry->items.at(binding->qualifiedId).parameters){auto control=controls.at(spec.id);if(auto check=qobject_cast<QCheckBox *>(control))values[spec.id]=check->isChecked();else if(auto combo=qobject_cast<QComboBox *>(control))values[spec.id]=combo->currentText().toStdString();else {const auto text=qobject_cast<QLineEdit *>(control)->text();if(spec.type==Providers::ParameterSpec::Type::String)values[spec.id]=text.toUtf8().toStdString();else if(spec.type==Providers::ParameterSpec::Type::Integer){bool okay=false;auto value=text.toLongLong(&okay);if(!okay)throw ProtocolError("Enter a valid integer for "+spec.label);values[spec.id]=int64_t(value);}else{bool okay=false;auto value=text.toDouble(&okay);if(!okay || !std::isfinite(value))throw ProtocolError("Enter a finite number for "+spec.label);values[spec.id]=value;}}}
    Providers::SymbolBindings names;std::set<std::string> reservations;for(const auto& helper:registry->items.at(binding->qualifiedId).recipes[binding->recipeIndex].helpers)names.emplace(helper.id,inventory->reserve("helper",reservations));auto generated=Providers::ProviderRegistry::generate(*binding,values,names);if(!generated)throw ProtocolError(generated.diagnostic->message);fragment=std::move(*generated.value);if(target && !placement){std::string owner;if(fragment->insertionRole==Providers::InsertionRole::Component && scene->semantic){auto found=scene->semantic->partOwners.find(*hit->object.nodeIndex);if(found!=scene->semantic->partOwners.end())owner=found->second;}auto captured=PlacementDraft::capture(*target,*fragment,hit->worldPoint,hit->outwardNormal,cameraUp,scene->semantic,owner);if(!captured)throw ProtocolError(captured.diagnostic->message);placement=std::move(*captured.value);refreshControls();}refreshScope();schedule(true);
  }catch(const std::exception& error){if(display){display.showBase();display.setPending(tr("Invalid insertion parameters"));}setState(State::Configuring,QString::fromUtf8(error.what()));}}
void Controller::refreshControls(){if(!placement)return;QScopedValueRollback<bool> guard(refreshing,true);for(int axis=0;axis<3;++axis)offsets[axis]->setValue(placement->offset[axis]);spin->setValue(placement->spinDegrees);flip->setChecked(placement->flipped);}
bool Controller::setPlacement(const Vector3d& offset,double degrees,bool flipped){if(!placement || !offset.allFinite() || !std::isfinite(degrees))return false;placement->offset=offset;placement->spinDegrees=degrees;placement->flipped=flipped;refreshControls();schedule(true);return true;}
bool Controller::setNamedAnchor(std::optional<Anchors::ReferenceKey> key){if(!placement || !fragment || fragment->insertionRole!=Providers::InsertionRole::Component)return false;placement->namedTarget=key;placement->mode=key?PlacementDraft::Mode::NamedAttachment:PlacementDraft::Mode::FixedComponent;refreshScope();schedule(true);return true;}
void Controller::approveShared(bool yes){{QSignalBlocker guard(shared);shared->setChecked(yes);}schedule();}
bool Controller::catalogsCurrent()const{try{auto resources=PlatformUtils::resourcePath("providers");auto live=Providers::discoverProviders({resources.empty()?"":EvaluationInputs::fileKey(resources),PlatformUtils::userConfigPath(),EvaluationInputs::fileKey(scene->backing->parsedRoot->modulePath()),{}});if(live.catalogs.paths!=discovered.catalogs.paths || live.libraryBindings.size()!=discovered.libraryBindings.size())return false;for(const auto& [id,value]:live.libraryBindings){auto old=discovered.libraryBindings.find(id);if(old==discovered.libraryBindings.end() || old->second.root!=value.root || old->second.importPrefix!=value.importPrefix)return false;}auto snapshot=Providers::ProviderRegistry::load(live.catalogs);if(snapshot->providers.size()!=registry->providers.size() || snapshot->items.size()!=registry->items.size() || snapshot->diagnostics.size()!=registry->diagnostics.size())return false;for(size_t i=0;i<snapshot->providers.size();++i)if(snapshot->providers[i].path!=registry->providers[i].path || snapshot->providers[i].digest!=registry->providers[i].digest)return false;return true;}catch(...){return false;}}
bool Controller::current(bool provider)const{try{if(!window || !rootEditor || !scene || !window->evaluationBinding->validateModel(stamp) || !Selection::EvaluationBinding::inputsCurrent(*scene->inputs,*window) || !capturedInputs || !Selection::EvaluationBinding::inputsCurrent(*capturedInputs,*window) || configurationHash(captureConfiguration(*scene->backing->builtin,rootEditor->parameterWidget->evaluationValues()))!=configurationHash(configuration) || !catalogsCurrent())return false;if(target && (!targetEditor || targetEditor->isSourceReadOnly() || !window->tabManager->editorList.contains(targetEditor)))return false;
    if(provider && binding){auto fresh=Selection::EvaluationBinding::capture(*window,*rootEditor);auto inputs=std::make_shared<EvaluationInputs>(fresh->root(),fresh->capturedEditors(),EvaluationInputs::ReadPolicy::CaptureMissing,scene->inputs->consumed());auto checked=Providers::ProviderRegistry::revalidate(*binding,context(target?directory(*target,*scene):bindingContext.sourceDirectory,inputs),registry);if(!checked)return false;}return true;
  }catch(...){return false;}}
void Controller::schedule(bool base){debounce.stop();stopProcess();++serial;result.reset();plan.reset();if(!target || !fragment || !placement)return;if(target->shared && !shared->isChecked()){if(display){display.showBase();display.setPending(tr("Shared scope needs approval"));}setState(State::Configuring,tr("Approve editing every listed instance of this reusable definition"));return;}
  if(!display){QString detail;if(!window->evaluationBinding->validateModel(stamp)){cancel();return;}display=window->inspectionPresentation->beginDraft("Insertion",this,window->evaluationBinding->stamp(),&detail);if(!display){setState(State::Configuring,detail);return;}overlay=window->viewportOverlays->registerOverlay(this,Selection::OverlayLayer::WorldDepth,100,[this](const auto&){drawGhost();});}
  if(base)display.showBase();display.setPending(tr("Full insertion preview · Pending · not committed"));setState(State::Pending,tr("Placement preview · Pending"));window->qglview->update();debounce.start();}
void Controller::launch(){if(!target || !fragment || !placement || !display)return;if(!current(true)){cancel();return;}try{
    Result<EditPlan> planned;
    if(fragment->insertionRole==Providers::InsertionRole::Component)planned=InsertionPlanner::component(*inventory,*target,*fragment,*placement,shared->isChecked());
    else {auto local=placement->localPlacement(*fragment);if(!local)throw ProtocolError(local.diagnostic->message);planned=InsertionPlanner::geometry(*inventory,*target,*fragment,*local.value,shared->isChecked());}
    if(!planned)throw ProtocolError(planned.diagnostic->message);plan=std::move(*planned.value);auto request=InsertionPlanner::preview(*plan,serial,executableIdentity(),EvaluationInputs::fileKey(scene->backing->parsedRoot->modulePath()),configuration,*capturedInputs);const auto ticket=serial;QPointer<Controller> self(this);process=new PreviewProcess(std::move(request),this,[self,ticket]{return self && self->serial==ticket && self->current();},[self](const std::string& key){if(!self || !self->current(true))throw ProtocolError("Captured insertion inputs changed");auto fresh=Selection::EvaluationBinding::capture(*self->window,*self->rootEditor);auto value=fresh->read(std::filesystem::u8path(key));const auto captured=self->capturedInputs->read(std::filesystem::u8path(key));if(captured.utf8!=value.utf8 || captured.editorId!=value.editorId || captured.editorRevision!=value.editorRevision || !self->current(true))throw ProtocolError("Input changed during insertion dependency capture");return value;},[self,ticket](auto value){if(self)self->accept(ticket,std::move(value));});process->start();elapsed.start();setState(State::Pending,tr("Full insertion preview · evaluating · Commit unavailable"));}catch(const std::exception& error){result.reset();display.showBase();display.setPending(tr("Insertion preview failed"));setState(State::Configuring,QString::fromUtf8(error.what()));}}
void Controller::accept(uint64_t ticket,PreviewResponse response){if(ticket!=serial || currentState==State::Idle || !plan)return;elapsed.stop();if(!current(true)){cancel();return;}try{validateProof(process->request(),response);if(response.status!=PreviewStatus::Success){QString text;for(const auto& diagnostic:response.diagnostics)text+=qs(diagnostic.message)+"\n";display.showBase();display.setPending(tr("Insertion unavailable"));setState(State::Configuring,text);return;}if(!display.showCandidate(ticket,response.meshes,tr("Full insertion preview · not committed")))throw ProtocolError("Candidate display no longer matches the model");ghost=response.itemMeshes;result=std::move(response);setState(State::Ready,tr("Insertion preview — not committed"));window->qglview->update();}catch(const std::exception& error){result.reset();display.showBase();setState(State::Configuring,QString::fromUtf8(error.what()));}}
bool Controller::openSource(size_t index){
  if(!inventory || index>=targetOptions.size() || !window->evaluationBinding->validateModel(stamp))return false;
  const auto& option=targetOptions[index];
  auto source=option.target?std::optional<SourceProvenance::Span>(option.target->region):option.diagnostic?option.diagnostic->source:std::nullopt;
  if(!source){window->statusBar()->showMessage(tr("No proven source range is available"),5000);return false;}
  auto provenance=inventory->sourceProvenance();SourceProvenance::Site site;site.whole=*source;site.wholeComplete=true;
  cancel();QString detail;
  const auto okay=SourceProvenance::ProvenanceService::forWindow(window)->navigateSite(provenance->syntaxTable(),site,window->evaluationBinding->stamp(),&detail);
  window->statusBar()->showMessage(okay?tr("Source opened. Evaluate the intended root and restart Add to select an editable destination."):detail,10000);
  return okay;
}
void Controller::reloadProviders(){cancel();registry.reset();window->statusBar()->showMessage(tr("Providers will reload when Add starts again"),5000);}
void Controller::armDrag(Drag next){if(!placement || !display)return;drag=next;setState(currentState,next==Drag::Move?tr("Click and drag in the placement plane"):tr("Click and drag around the placement origin"));}
Result<Vector2d> Controller::planePoint(QPointF pixel)const{if(!placement || !target)return {{},Diagnostic{"NoPlacement","Choose a placement first",{}}};auto view=window->qglview;view->makeCurrent();view->setupCamera();auto physical=SurfacePickMath::pixel(pixel.x(),pixel.y(),view->devicePixelRatio(),view->cam.pixel_width,view->cam.pixel_height);const Eigen::Map<const Eigen::Matrix4d> projection(view->projection),modelview(view->modelview);auto rayNear=physical?SurfacePickMath::unproject(physical->x(),physical->y(),0,view->cam.pixel_width,view->cam.pixel_height,projection,modelview,Vector3d::Zero()):std::nullopt;auto rayFar=physical?SurfacePickMath::unproject(physical->x(),physical->y(),0.9999,view->cam.pixel_width,view->cam.pixel_height,projection,modelview,Vector3d::Zero()):std::nullopt;view->doneCurrent();if(!rayNear || !rayFar)return {{},Diagnostic{"CameraRay","Camera ray is unavailable",{}}};return placement->planeCoordinates(*target,scene->semantic,*rayNear,*rayFar-*rayNear);}
bool Controller::startDrag(QPointF point){const auto start=planePoint(point);if(!start){status->setText(qs(start.diagnostic->message));return false;}const auto mode=drag;const auto initial=*placement;const auto first=*start.value;const auto firstAngle=PlacementDraft::spinAngle(first,initial.offset.head<2>());if(mode==Drag::Spin && !firstAngle){status->setText(tr("Move away from the spin origin"));return false;}auto update=[this,mode,initial,first,firstAngle](QPointF point,auto){if(!placement)return;auto current=planePoint(point);if(!current){status->setText(qs(current.diagnostic->message));return;}if(mode==Drag::Move){Vector3d offset=initial.offset;offset.head<2>()+=*current.value-first;setPlacement(offset,initial.spinDegrees,initial.flipped);}else if(auto angle=PlacementDraft::spinAngle(*current.value,initial.offset.head<2>())){const auto delta=std::remainder(*angle-*firstAngle,360.0);setPlacement(initial.offset,initial.spinDegrees+delta,initial.flipped);}};
  if(!window->viewportTools->beginSessionDrag(tool,{"Insertion",stamp,false},point,{update,[this,update](QPointF point,auto modifiers){update(point,modifiers);drag=Drag::None;},[this](auto){cancel();}}))return false;display.showBase();return true;}
bool Controller::eventFilter(QObject *object,QEvent *event){if(currentState==State::Idle || currentState==State::Picking)return false;auto widget=qobject_cast<QWidget *>(object);const bool internal=widget && (widget==dock || dock->isAncestorOf(widget));if(internal && event->type()==QEvent::KeyPress){auto key=static_cast<QKeyEvent *>(event);if(key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter){if(qobject_cast<QLineEdit *>(widget) || qobject_cast<QDoubleSpinBox *>(widget) || (widget->parentWidget() && qobject_cast<QDoubleSpinBox *>(widget->parentWidget()))){widget->clearFocus();window->qglview->setFocus();return true;}}}
  if(object==window->qglview && event->type()==QEvent::MouseButtonPress && drag!=Drag::None){auto mouse=static_cast<QMouseEvent *>(event);if(mouse->button()==Qt::LeftButton && mouse->modifiers()==Qt::NoModifier)return startDrag(mouse->position());}return false;}
void Controller::drawGhost(){if(!ghost || !placement || !fragment || !target || currentState!=State::Pending || window->evaluationBinding->draftPixels())return;auto world=placement->worldPlacement(*target,*fragment,scene->semantic);if(!world)return;ScopedGLState state;glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glColor4f(1.f,0.65f,0.1f,0.45f);glBegin(GL_TRIANGLES);for(const auto& piece:ghost->pieces)for(const auto& face:piece.mesh->indices)for(auto index:face){const Vector3d vertex=*world.value*piece.localToWorld*piece.mesh->vertices[index];glVertex3dv(vertex.data());}glEnd();}
bool Controller::commit(){
  if(currentState!=State::Ready || !plan || !result || !process || !targetEditor || !rootEditor || !target ||
     result->status!=PreviewStatus::Success || result->serial!=serial || result->candidateHash!=plan->candidateHash ||
     EvaluationInputs::fingerprint(plan->candidate)!=plan->candidateHash || (target->shared && !plan->sharedApproved))return false;
  try {
    validateProof(process->request(),*result);
    if(!current(true))throw ProtocolError("Insertion input proof is stale; restart from a current surface");
    const auto& original=inventory->input(target->body.fileKey);
    if(sourceKey(targetEditor)!=original.fileKey || !original.editorId || !original.editorRevision ||
       targetEditor->documentId().toStdString()!=*original.editorId || targetEditor->sourceRevision!=*original.editorRevision ||
       targetEditor->sourceFingerprint.toStdString()!=original.fingerprint || targetEditor->toPlainText().toUtf8().toStdString()!=original.utf8)
      throw ProtocolError("The explicitly selected source editor no longer matches its captured bytes");
    SourceEditing::EditRequest transaction{targetEditor->documentId(),targetEditor->sourceRevision,targetEditor->sourceFingerprint,{}};
    for(const auto& patch:plan->patches)transaction.edits.append({qsizetype(patch.edit.begin),qsizetype(patch.edit.end),QByteArray::fromStdString(patch.expected),QByteArray::fromStdString(patch.edit.replacement)});
    QPointer<Controller> self(this);const auto ticket=serial;const auto hash=plan->candidateHash;
    transaction.contextCurrent=[self,ticket,hash]{return self && self->currentState==State::Committing && self->serial==ticket && self->plan && self->result && self->plan->candidateHash==hash && self->result->candidateHash==hash && self->current(true);};
    const auto capturedConfiguration=configuration;QPointer<MainWindow> owner=window;QPointer<EditorInterface> root=rootEditor;
    setState(State::Committing,tr("Committing insertion"));
    // SourceEditing rechecks this context after synchronous pre-edit observers. No event loop is entered here.
    const auto applied=SourceEditing::apply(*targetEditor,transaction);
    if(!self)return applied.error==SourceEditing::EditError::None;
    if(applied.error!=SourceEditing::EditError::None){cancel();if(owner)owner->statusBar()->showMessage(applied.detail,10000);return false;}
    cancel();
    if(owner && root && owner->activeEditor==root){owner->statusBar()->showMessage(tr("Committed; evaluating"));if(!owner->evaluateCapturedVariables(capturedConfiguration.variables,capturedConfiguration.parameters))owner->statusBar()->showMessage(tr("Source committed; evaluation could not start. Undo remains available."),10000);}
    return true;
  }catch(const std::exception& error){const auto message=QString::fromUtf8(error.what());cancel();if(window)window->statusBar()->showMessage(message,10000);return false;}
}
}
