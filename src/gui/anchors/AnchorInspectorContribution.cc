#include "gui/anchors/AnchorController.h"
#include "gui/MainWindow.h"
#include "gui/selection/EvaluationBinding.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <array>
namespace Anchors {
namespace {
QString vector(const Vector3d& value){return QString("[%1, %2, %3]").arg(value.x(),0,'g',10).arg(value.y(),0,'g',10).arg(value.z(),0,'g',10);}
Value value(const Vector3d& vector){VectorType out(nullptr);for(int i=0;i<3;++i)out.emplace_back(vector[i]);return Value(std::move(out));}
LiteralChange field(std::string path,Value value){return {std::move(path),std::make_shared<const Value>(Design::ownedValue(value))};}
}
QWidget *Controller::buildInspector(){
  auto *body=new QWidget;body->setObjectName("anchorInspector");auto *layout=new QVBoxLayout(body);layout->setContentsMargins(0,0,0,0);
  auto button=[&](QString text,const char *name,bool enabled,std::function<void()> run){auto *button=new QPushButton(text,body);button->setObjectName(name);button->setEnabled(enabled);layout->addWidget(button);connect(button,&QPushButton::clicked,this,std::move(run));return button;};
  const bool current=window && window->evaluationBinding->current() && service;
  button(tr("Pick anchor"),"pickAnchor",current,[this]{beginPick();});
  auto draft=captureDraft();
  if(draft){auto payload=std::dynamic_pointer_cast<const ReferenceDeclaration>(draft->preview->evaluated);auto key=referenceKey(draft->key);if(payload && key){auto anchor=std::get<Anchor>(payload->value);auto label=new QLabel(tr("Fixed capture · unsaved\nOwner: %1\nLocal position: %2\nNormal: %3\nUp: %4").arg(QString::fromStdString(key->owner.id),vector(anchor.position),vector(anchor.normal),vector(anchor.up)),body);label->setObjectName("capturedAnchorStatus");label->setWordWrap(true);layout->addWidget(label);}
    button(tr("Save anchor"),"saveAnchor",current,[this]{saveCapture();});button(tr("Cancel capture"),"cancelAnchorCapture",true,[this]{cancelCapture();});
  }
  auto refs=listedReferences();auto *chooser=new QComboBox(body);chooser->setObjectName("anchorReferenceList");chooser->addItem(tr("Select reference"));
  if(refs){for(const auto& [key,reference]:refs->references)chooser->addItem(QString::fromStdString(reference.declaration->kind+" · "+reference.label+" · "+key.owner.id),QString::fromStdString(opaqueKey(key)));
    for(const auto& [key,attachment]:refs->attachments)chooser->addItem(QString::fromStdString("Attachment · "+attachment.label+" · "+key.owner.id),QString::fromStdString(opaqueKey(key)));
    for(const auto& key:refs->invalidReferences)chooser->addItem(QString::fromStdString("Invalid reference · "+key.id+" · "+key.owner.id),QString::fromStdString(opaqueKey(key)));
  }
  if(selected)for(int i=1;i<chooser->count();++i)if(chooser->itemData(i).toString().toStdString()==opaqueKey(*selected))chooser->setCurrentIndex(i);
  layout->addWidget(chooser);chooser->setEnabled(current);
  connect(chooser,&QComboBox::currentIndexChanged,this,[this,chooser](int row){if(row>0)if(auto key=referenceKey(chooser->itemData(row).toString().toStdString()))selectReference(*key);});
  if(!selected){layout->addWidget(new QLabel(current?tr("Named references retain their declared type and owner."):tr("References are stale or unavailable."),body));return body;}
  const auto key=*selected;auto resolved=resolveForDisplay(key);auto declaration=record(key);
  QString text=tr("Owner: %1\nID: %2\nState: %3").arg(QString::fromStdString(key.owner.id),QString::fromStdString(key.id),resolved.state==Design::ResolutionState::Resolved?tr("Resolved"):QString::fromStdString(resolved.diagnostic));
  if(declaration){text+=tr("\nType: %1\nLabel: %2").arg(QString::fromStdString(declaration->kind),QString::fromStdString(declaration->label));
    if(declaration->source)text+=tr("\nSource: %1\nDeclaration uses: %2").arg(QString::fromStdString(declaration->source->fileKey)).arg(declarationUses(*declaration));
    if(auto payload=std::dynamic_pointer_cast<const ReferenceDeclaration>(declaration->evaluated);payload && std::holds_alternative<Anchor>(payload->value))text+=std::get<Anchor>(payload->value).fixedCapture?tr("\nFixed local capture"):tr("\nParametric declaration");
    if(declaration->kind=="Attachment")text+=tr("\nAttachment relation evaluated in the complete model.");
  }
  if(resolved.world)std::visit([&](const auto& value){text+=tr("\nWorld position: %1").arg(vector(value.position));},*resolved.world);
  if(resolved.frame)text+=tr("\nNormal: %1\nUp: %2").arg(vector(resolved.frame->linear().col(2)),vector(resolved.frame->linear().col(1)));
  auto *status=new QLabel(text,body);status->setObjectName("anchorReferenceStatus");status->setWordWrap(true);status->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(status);
  if(!declaration)return body;
  button(tr("Go to declaration"),"anchorGoToSource",current && declaration->source.has_value(),[this,key]{navigate(key);});
  const auto uses=declarationUses(*declaration);if(uses>1)button(tr("Edit shared declaration (%1 uses)").arg(uses),"anchorEditShared",current && !sharedEdits.count(key),[this,key]{allowSharedEdit(key);});
  std::string error;const auto stamp=window->evaluationBinding->stamp();auto editable=planEdit(key,{field("label",Value(declaration->label))},stamp);
  if(!editable){auto *sourceOnly=new QLabel(QString::fromStdString(editable.error),body);sourceOnly->setObjectName("anchorSourceOnly");sourceOnly->setWordWrap(true);layout->addWidget(sourceOnly);}
  auto *form=new QFormLayout;layout->addLayout(form);auto *label=new QLineEdit(QString::fromStdString(declaration->label),body);label->setObjectName("anchorLabel");form->addRow(tr("Label"),label);
  std::array<QDoubleSpinBox *,3> translation{};std::array<QDoubleSpinBox *,4> quaternion{};QDoubleSpinBox *spin=nullptr;QComboBox *mode=nullptr;
  auto number=[&](double initial,const std::string& name){auto *control=new QDoubleSpinBox(body);control->setObjectName(QString::fromStdString(name));control->setDecimals(10);control->setRange(-1e100,1e100);control->setValue(initial);control->setEnabled(bool(editable));return control;};
  if(auto anchor=std::dynamic_pointer_cast<const ReferenceDeclaration>(declaration->evaluated);anchor && std::holds_alternative<Anchor>(anchor->value)){
    const auto& value=std::get<Anchor>(anchor->value);for(int i=0;i<3;++i){translation[i]=number(value.translation[i],"anchorTranslation"+std::to_string(i));form->addRow(tr("Translation %1").arg(i+1),translation[i]);}
    const double q[4]={value.rotation.x(),value.rotation.y(),value.rotation.z(),value.rotation.w()};for(int i=0;i<4;++i){quaternion[i]=number(q[i],"anchorRotation"+std::to_string(i));form->addRow(tr("Quaternion %1").arg(QString("xyzw")[i]),quaternion[i]);}
  } else if(auto attachment=std::dynamic_pointer_cast<const AttachmentDeclaration>(declaration->evaluated)){
    for(int i=0;i<3;++i){translation[i]=number(attachment->offsetAdjustment[i],"attachmentOffset"+std::to_string(i));form->addRow(tr("Offset adjustment %1").arg(i+1),translation[i]);}
    spin=number(attachment->spinAdjustment,"attachmentSpin");form->addRow(tr("Spin adjustment"),spin);
    mode=new QComboBox(body);mode->setObjectName("attachmentMode");mode->addItem(tr("Mate"),"mate");mode->addItem(tr("Align"),"align");mode->setCurrentIndex(attachment->mode==AttachmentMode::Mate?0:1);mode->setEnabled(bool(editable));form->addRow(tr("Mode"),mode);
  }
  label->setEnabled(bool(editable));
  button(tr("Apply reference changes"),"applyAnchorChanges",bool(editable),[this,key,label,translation,quaternion,spin,mode]{
    std::vector<LiteralChange> fields{field("label",Value(label->text().toStdString()))};
    if(translation[0])fields.push_back(field(mode?"adjust.offset":"adjust.translation",value(Vector3d(translation[0]->value(),translation[1]->value(),translation[2]->value()))));
    if(quaternion[0]){VectorType q(nullptr);for(auto *control:quaternion)q.emplace_back(control->value());fields.push_back(field("adjust.rotation",Value(std::move(q))));}
    if(spin)fields.push_back(field("adjust.spin",Value(spin->value())));if(mode)fields.push_back(field("mode",Value(mode->currentData().toString().toStdString())));
    auto result=editReference(key,fields);report(result.error);
  });
  return body;
}
}
