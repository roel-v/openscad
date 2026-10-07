#include "gui/semantic/SemanticParameterPanel.h"
#include "gui/semantic/SemanticController.h"
#include "gui/MainWindow.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/ProvenanceService.h"
#include "gui/source/SourceEditTransaction.h"
#include "gui/parameter/ParameterCheckBox.h"
#include "gui/parameter/ParameterSpinBox.h"
#include "gui/parameter/ParameterText.h"
#include "gui/parameter/ParameterVector.h"
#include "core/EvaluationInputs.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QTimer>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QFileInfo>
#include <limits>
namespace Semantic {
ParameterPanel::ParameterPanel(MainWindow *owner,Controller *control,QWidget *parent):QWidget(parent),window(owner),controller(control) {
  setObjectName("semanticParameterPanel");auto *layout=new QVBoxLayout(this);
  setAccessibleName(tr("Component parameters"));
  auto *heading=new QLabel(tr("Component parameters"),this);auto font=heading->font();font.setBold(true);heading->setFont(font);layout->addWidget(heading);
  setMinimumHeight(260);
  mode=new QComboBox(this);mode->setObjectName("semanticParameterMode");
  mode->addItems({tr("Selected component"),tr("Module defaults"),tr("All components (read-only)")});layout->addWidget(mode);
  status=new QLabel(this);status->setObjectName("semanticParameterStatus");status->setWordWrap(true);layout->addWidget(status);
  scroll=new QScrollArea(this);scroll->setWidgetResizable(true);layout->addWidget(scroll);
  connect(mode,&QComboBox::currentIndexChanged,this,[this]{refresh();});
}
EditorInterface *ParameterPanel::owner(const Scene& scene,const ParameterEdit& edit,QString& reason) const {
  if(!window || !scene.inputs || !edit.owner){reason=tr("Captured source owner is unavailable");return nullptr;}
  const auto input=scene.inputs->consumed().find(edit.owner->whole.fileKey);
  if(input==scene.inputs->consumed().end() || !input->second.editorId || !input->second.editorRevision){reason=tr("Open this source explicitly and reevaluate before editing it");return nullptr;}
  EditorInterface *matched=nullptr;
  for(auto *editor:window->tabManager->editorList)if(editor->documentId().toStdString()==*input->second.editorId){matched=editor;break;}
  if(!matched){reason=tr("Captured source editor has closed");return nullptr;}
  if(matched->isSourceReadOnly()){reason=tr("Source editor is read-only");return nullptr;}
  if(matched->sourceRevision!=*input->second.editorRevision || matched->sourceFingerprint.toStdString()!=input->second.fingerprint){reason=tr("Captured source changed; reevaluate");return nullptr;}
  const auto key=matched->filepath.isEmpty()?"session:"+matched->documentId().toStdString():EvaluationInputs::fileKey(matched->filepath.toStdString());
  if(key!=input->first){reason=tr("Source editor identity changed");return nullptr;}
  return matched;
}
bool ParameterPanel::applyCaptured(std::shared_ptr<const Selection::EvaluatedScene> captured,Selection::Stamp stamp,TargetKey key,
                                   const Parameter& parameter,const Value& value,bool reset,QString *detail) {
  auto fail=[&](QString reason){if(detail)*detail=reason;if(status)status->setText(reason);return false;};
  if(!window || !controller || !captured || !captured->semantic || !controller->targetService()->validate(stamp))return fail(tr("Model snapshot changed; reevaluate before editing"));
  if(window->evaluationBinding->scene(window->evaluationBinding->stamp())!=captured)return fail(tr("Model result changed"));
  auto edit=reset?resetParameter(*captured->semantic,key,parameter):changeParameter(*captured->semantic,key,parameter,value);
  if(!edit.editable)return fail(QString::fromStdString(edit.reason));
  QString reason;QPointer<EditorInterface> source=owner(*captured->semantic,edit,reason);if(!source)return fail(reason);
  QPointer<MainWindow> weakWindow=window;QPointer<TargetService> weakService=controller->targetService();QPointer<EditorInterface> root=window->activeEditor;
  QPointer<Controller> weakController=controller;const auto selected=controller->selectedTarget();
  SourceEditing::EditRequest request{source->documentId(),source->sourceRevision,source->sourceFingerprint,{},{}};
  request.contextCurrent=[weakWindow,weakService,weakController,selected,root,captured,stamp]{return weakWindow && weakService && weakController && weakController->selectedTarget()==selected && root && weakWindow->activeEditor==root && weakService->validate(stamp) && weakWindow->evaluationBinding->scene(weakWindow->evaluationBinding->stamp())==captured;};
  for(const auto& patch:edit.patches)request.edits.append({static_cast<qsizetype>(patch.span.begin),static_cast<qsizetype>(patch.span.end),QByteArray::fromStdString(patch.expected),QByteArray::fromStdString(patch.replacement)});
  QPointer<ParameterPanel> self(this);
  const auto result=SourceEditing::apply(*source,request);
  if(!self)return false;
  if(result.error!=SourceEditing::EditError::None)return fail(result.detail);
  if(result.changed){
    status->setText(tr("Source changed; reevaluating selected target"));
    QTimer::singleShot(0,window,[weakWindow,root]{if(weakWindow && root && weakWindow->activeEditor==root)weakWindow->actionRenderPreview();});
  }
  return true;
}
void ParameterPanel::initializeStructureDialog() {
  if(!window || !window->activeEditor)return;
  const auto stamp=window->evaluationBinding->stamp();const auto captured=window->evaluationBinding->scene(stamp);
  const auto filename=QFileInfo(window->activeEditor->filepath).completeBaseName();
  QPointer<ParameterPanel> self(this);bool accepted=false;
  const auto name=QInputDialog::getText(window,tr("Add model structure"),
    tr("Part name (global parameters remain unchanged):"),QLineEdit::Normal,filename.isEmpty()?tr("Model"):filename,&accepted);
  if(self && accepted){QString detail;if(!self->initializeCaptured(captured,stamp,name.trimmed(),&detail) && self)QMessageBox::information(self->window,tr("Cannot add model structure"),detail);}
}
bool ParameterPanel::initializeCaptured(std::shared_ptr<const Selection::EvaluatedScene> captured,Selection::Stamp stamp,
                                       const QString& name,QString *detail) {
  auto fail=[&](QString reason){if(detail)*detail=reason;status->setText(reason);return false;};
  if(!window || !window->activeEditor || !captured || !captured->semantic || !captured->semantic->inputs ||
     !window->evaluationBinding->validate(stamp) || window->evaluationBinding->scene(stamp)!=captured)
    return fail(tr("The model changed. Preview the current code before adding structure."));
  QPointer<EditorInterface> editor=window->activeEditor;const auto& input=captured->semantic->inputs->root();
  if(!input.editorId || !input.editorRevision || editor->documentId().toStdString()!=*input.editorId ||
     editor->sourceRevision!=*input.editorRevision || editor->sourceFingerprint.toStdString()!=input.fingerprint)
    return fail(tr("The source changed. Preview again before adding structure."));
  if(editor->isSourceReadOnly())return fail(tr("This source is read-only."));
  const auto edit=Semantic::initializeStructure(*captured->semantic,input.fileKey,name.trimmed().toStdString());
  if(!edit.editable)return fail(QString::fromStdString(edit.reason));
  QPointer<MainWindow> ownerWindow=window;QPointer<ParameterPanel> self(this);
  SourceEditing::EditRequest request{editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{},{}};
  request.contextCurrent=[ownerWindow,editor,captured,stamp]{return ownerWindow && editor && ownerWindow->activeEditor==editor &&
    ownerWindow->evaluationBinding->validate(stamp) && ownerWindow->evaluationBinding->scene(stamp)==captured;};
  for(const auto& patch:edit.patches)request.edits.append({static_cast<qsizetype>(patch.span.begin),static_cast<qsizetype>(patch.span.end),QByteArray::fromStdString(patch.expected),QByteArray::fromStdString(patch.replacement)});
  const auto result=SourceEditing::apply(*editor,request);
  if(!self)return false;
  if(result.error!=SourceEditing::EditError::None)return fail(result.detail);
  if(result.changed){status->setText(tr("Model structure added. Previewing…"));QTimer::singleShot(0,window,[ownerWindow,editor]{if(ownerWindow && editor && ownerWindow->activeEditor==editor)ownerWindow->actionRenderPreview();});}
  return result.changed;
}
void ParameterPanel::refresh() {
  if(rebuilding || !window || !controller)return;QScopedValueRollback<bool> guard(rebuilding,true);
  auto *body=new QWidget;body->setLayout(new QVBoxLayout);body->layout()->setContentsMargins(4,4,4,4);
  delete scroll->takeWidget();scroll->setWidget(body);
  const auto stamp=window->evaluationBinding->stamp();const auto captured=window->evaluationBinding->displayScene(stamp);
  if(!captured || !captured->semantic || !controller->targetService()->displayCurrent(stamp)){
    status->setText(tr("Preview the current code (F5) to inspect component parameters."));return;
  }
  if(!captured->semantic->valid()){
    status->setText(tr("Part or assembly declarations contain errors. Check the model structure and source diagnostics."));return;
  }
  if(!captured->semantic->marked){
    status->setText(tr("This model has no component structure yet. Add a single part to start organizing it. Global parameters remain available."));
    auto *initialize=new QPushButton(tr("Add model structure…"),body);initialize->setObjectName("semanticInitializeStructure");body->layout()->addWidget(initialize);
    initialize->setEnabled(window->activeEditor && !window->activeEditor->isSourceReadOnly());
    if(!initialize->isEnabled())status->setText(tr("This model has no component structure. Its source is read-only."));
    connect(initialize,&QPushButton::clicked,this,&ParameterPanel::initializeStructureDialog);return;
  }
  auto selected=controller->selectedTarget();
  if(mode->currentIndex()==2){
    status->setText(tr("All evaluated instance parameters. Select a target to edit its unique call."));
    for(const auto& [path,instance]:captured->semantic->instances){
      auto *label=new QLabel(QString::fromStdString(path),body);label->setTextInteractionFlags(Qt::TextSelectableByMouse);body->layout()->addWidget(label);
      for(const auto& [name,parameter]:instance.parameters)addParameter(body,{TargetRole::Instance,path},parameter,true,captured,stamp);
    }
  }else if(!selected){status->setText(tr("Select a component or module definition in Model structure."));}
  else{
    if(mode->currentIndex()==1 && selected->role==TargetRole::Instance)selected=TargetKey{TargetRole::Definition,captured->semantic->instances.at(selected->id).definitionId};
    std::map<std::string,Parameter> parameters;
    if(selected->role==TargetRole::Instance){parameters=captured->semantic->instances.at(selected->id).parameters;status->setText(tr("Instance: %1\nEdits change its source call. Reset removes a named override.").arg(QString::fromStdString(selected->id)));}
    else{
      const auto target=controller->targetService()->resolveForDisplay(*selected);parameters=target.parameters;
      status->setText(tr("Definition defaults: %1\nEditing affects every use that inherits this default. Explicit call overrides remain.").arg(QString::fromStdString(selected->id))+(parameters.empty()?"\n"+QString::fromStdString(target.diagnostic):QString()));
    }
    for(const auto& [name,parameter]:parameters)addParameter(body,*selected,parameter,false,captured,stamp);
  }
  static_cast<QVBoxLayout *>(body->layout())->addStretch();
}
void ParameterPanel::addParameter(QWidget *body,TargetKey key,const Parameter& parameter,bool inspectionOnly,
                                  std::shared_ptr<const Selection::EvaluatedScene> captured,Selection::Stamp stamp) {
  auto *row=new QWidget(body);row->setObjectName("semanticParameter-"+QString::fromStdString(parameter.name));auto *layout=new QVBoxLayout(row);body->layout()->addWidget(row);
  const auto edit=inspectParameter(*captured->semantic,key,parameter);QString reason=QString::fromStdString(edit.reason);auto *source=owner(*captured->semantic,edit,reason);
  const bool editable=!inspectionOnly && edit.editable && source;
  const bool expression=parameter.source && parameter.source->hasExpression && !literalParameterSource(*captured->semantic,parameter);
  QString origin=key.role==TargetRole::Definition?tr("definition default"):parameter.fromDefault?tr("inherited default"):expression?tr("call expression"):tr("call override");
  QString sourceText;if(parameter.source && parameter.source->hasExpression)sourceText=QString::fromStdString(parameter.source->originalExpression);
  auto *label=new QLabel(QString::fromStdString(parameter.name)+" = "+QString::fromStdString(parameter.displayedValue)+"\n"+origin+(sourceText.isEmpty()?QString():" · "+sourceText),row);
  label->setWordWrap(true);label->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(label);
  if(edit.owner){const auto file=edit.owner->whole.fileKey.rfind("session:",0)==0?tr("Untitled document"):QString::fromStdString(edit.owner->whole.fileKey);auto *ownerLabel=new QLabel(tr("Source owner: %1").arg(file),row);ownerLabel->setWordWrap(true);ownerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(ownerLabel);}
  auto *buttons=new QWidget(row);auto *buttonLayout=new QHBoxLayout(buttons);buttonLayout->setContentsMargins(0,0,0,0);layout->addWidget(buttons);
  auto *apply=new QPushButton(key.role==TargetRole::Definition?tr("Apply default"):tr("Apply override"),buttons);apply->setObjectName("semanticApply-"+QString::fromStdString(parameter.name));apply->setEnabled(false);buttonLayout->addWidget(apply);
  std::vector<std::shared_ptr<ParameterObject>> objects;std::vector<ParameterVirtualWidget *> widgets;
  if(editable){
    auto add=[&](std::shared_ptr<ParameterObject> object,ParameterVirtualWidget *widget){
      objects.push_back(std::move(object));widgets.push_back(widget);layout->insertWidget(layout->count()-1,widget);
      std::vector<std::unique_ptr<QSignalBlocker>> blocked;
      for(auto *spin:widget->findChildren<QDoubleSpinBox *>()){blocked.push_back(std::make_unique<QSignalBlocker>(spin));spin->setDecimals(std::numeric_limits<double>::max_digits10);}
      objects.back()->reset();
      widget->setValue();connect(widget,&ParameterVirtualWidget::changed,row,[apply](bool){apply->setEnabled(true);});
    };
    const auto& value=*parameter.value;const auto name=parameter.name;
    if(parameter.type==Value::Type::BOOL){auto object=std::make_shared<BoolParameter>(name,"","",value.toBool());add(object,new ParameterCheckBox(row,object.get(),DescriptionStyle::HideDetails));}
    else if(parameter.type==Value::Type::NUMBER){auto object=std::make_shared<NumberParameter>(name,"","",value.toDouble(),boost::none,boost::none,boost::none);add(object,new ParameterSpinBox(row,object.get(),DescriptionStyle::HideDetails));}
    else if(parameter.type==Value::Type::STRING){auto object=std::make_shared<StringParameter>(name,"","",value.toString(),boost::none);add(object,new ParameterText(row,object.get(),DescriptionStyle::HideDetails));}
    else if(parameter.type==Value::Type::VECTOR){
      std::vector<double> values;for(const auto& item:value.toVector())values.push_back(item.toDouble());
      for(size_t start=0;start<values.size();start+=4){const auto end=std::min(values.size(),start+4);std::vector<double> chunk(values.begin()+start,values.begin()+end);
        auto object=std::make_shared<VectorParameter>(name,"","",chunk,boost::none,boost::none,boost::none);add(object,new ParameterVector(row,object.get(),DescriptionStyle::HideDetails));}
    }
    QPointer<QPushButton> weakApply=apply;std::vector<QPointer<ParameterVirtualWidget>> weakWidgets;for(auto *widget:widgets)weakWidgets.emplace_back(widget);
    connect(apply,&QPushButton::clicked,this,[this,captured,stamp,key,parameter,objects,weakWidgets,weakApply]{
      std::shared_ptr<const Value> value;
      if(parameter.type==Value::Type::BOOL)value=std::make_shared<const Value>(static_cast<BoolParameter *>(objects.front().get())->value);
      else if(parameter.type==Value::Type::NUMBER)value=std::make_shared<const Value>(static_cast<NumberParameter *>(objects.front().get())->value);
      else if(parameter.type==Value::Type::STRING)value=std::make_shared<const Value>(static_cast<StringParameter *>(objects.front().get())->value);
      else{VectorType vector(nullptr);for(const auto& object:objects)for(double item:static_cast<VectorParameter *>(object.get())->value)vector.emplace_back(item);value=std::make_shared<const Value>(std::move(vector));}
      if(applyCaptured(captured,stamp,key,parameter,*value)){for(auto widget:weakWidgets)if(widget)widget->valueApplied();if(weakApply)weakApply->setEnabled(false);}
    });
  }else{
    apply->hide();auto *explanation=new QLabel(inspectionOnly?tr("Inspection only"):reason,row);explanation->setWordWrap(true);layout->addWidget(explanation);
  }
  if(edit.removable && editable){auto *reset=new QPushButton(tr("Reset override"),buttons);reset->setObjectName("semanticReset-"+QString::fromStdString(parameter.name));buttonLayout->addWidget(reset);
    connect(reset,&QPushButton::clicked,this,[this,captured,stamp,key,parameter]{applyCaptured(captured,stamp,key,parameter,*parameter.value,true);});}
  if(edit.owner){auto *go=new QPushButton(tr("Source"),buttons);go->setObjectName("semanticParameterSource-"+QString::fromStdString(parameter.name));buttonLayout->addWidget(go);
    connect(go,&QPushButton::clicked,this,[this,captured,stamp,site=*edit.owner]{QString detail;if(window && controller && controller->targetService()->validate(stamp))SourceProvenance::ProvenanceService::forWindow(window)->navigateSite(*captured->semantic->syntax,site,window->evaluationBinding->stamp(),&detail);});}
}
}
