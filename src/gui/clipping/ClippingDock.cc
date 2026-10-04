#include "gui/clipping/ClippingDock.h"
#include "gui/clipping/ClippingController.h"
#include "gui/MainWindow.h"
#include "gui/TabManager.h"
#include "gui/selection/ViewportToolRouter.h"
#include <QTreeWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace Clipping {
Dock::Dock(Controller *controller,QWidget *parent):QDockWidget(tr("Sections"),parent),controller(controller) {
  setObjectName("clippingDock");auto body=new QWidget(this);auto layout=new QVBoxLayout(body);
  global=new QCheckBox(tr("Enable clipping"),body);global->setObjectName("sectionGlobal");layout->addWidget(global);
  auto presets=new QHBoxLayout;for(const auto& preset:{"XY","XZ","YZ"}){auto button=new QPushButton(tr("New %1").arg(preset),body);button->setObjectName("sectionPreset"+QString(preset));presets->addWidget(button);connect(button,&QPushButton::clicked,this,[this,preset]{this->controller->createPreset(preset);});}layout->addLayout(presets);
  rows=new QTreeWidget(body);rows->setObjectName("sectionRows");rows->setHeaderLabels({tr("Section"),tr("Backing"),tr("Status")});rows->setRootIsDecorated(false);layout->addWidget(rows);
  auto form=new QFormLayout;name=new QLineEdit(body);name->setObjectName("sectionName");form->addRow(tr("Name"),name);
  for(int axis=0;axis<3;++axis){origin[axis]=new QDoubleSpinBox(body);origin[axis]->setObjectName("sectionOrigin"+QString("XYZ").mid(axis,1));origin[axis]->setDecimals(8);origin[axis]->setRange(-1e100,1e100);origin[axis]->setKeyboardTracking(false);form->addRow(tr("Origin %1").arg(QString("XYZ").mid(axis,1)),origin[axis]);connect(origin[axis],&QDoubleSpinBox::editingFinished,this,[this]{if(updating)return;Vector3d point(origin[0]->value(),origin[1]->value(),origin[2]->value());this->controller->setOrigin(this->controller->selectedId(),point);});}
  offset=new QDoubleSpinBox(body);offset->setObjectName("sectionOffset");offset->setDecimals(8);offset->setRange(-1e100,1e100);offset->setKeyboardTracking(false);form->addRow(tr("Offset"),offset);
  slider=new QSlider(Qt::Horizontal,body);slider->setObjectName("sectionOffsetSlider");slider->setRange(0,1000);form->addRow(tr("Model range"),slider);
  azimuth=new QDoubleSpinBox(body);azimuth->setObjectName("sectionAzimuth");azimuth->setRange(-180,180);azimuth->setDecimals(3);azimuth->setKeyboardTracking(false);form->addRow(tr("Azimuth"),azimuth);
  elevation=new QDoubleSpinBox(body);elevation->setObjectName("sectionElevation");elevation->setRange(-90,90);elevation->setDecimals(3);elevation->setKeyboardTracking(false);form->addRow(tr("Elevation"),elevation);layout->addLayout(form);
  helper=new QCheckBox(tr("Show selected plane helper"),body);helper->setObjectName("sectionHelper");layout->addWidget(helper);
  auto actions=new QHBoxLayout;
  auto button=[&](QString label,const char *object){auto result=new QPushButton(label,body);result->setObjectName(object);actions->addWidget(result);return result;};
  pickPoint=button(tr("Pick point"),"sectionPickPoint");pickAlign=button(tr("Pick + align"),"sectionPickAlign");flip=button(tr("Flip"),"sectionFlip");layout->addLayout(actions);
  auto reset=new QHBoxLayout;for(const auto& preset:{"XY","XZ","YZ"}){auto button=new QPushButton(tr("Reset %1").arg(preset),body);button->setObjectName("sectionReset"+QString(preset));reset->addWidget(button);connect(button,&QPushButton::clicked,this,[this,preset]{this->controller->reset(this->controller->selectedId(),preset);});}layout->addLayout(reset);
  auto persistence=new QHBoxLayout;promote=new QPushButton(tr("Promote to source"),body);promote->setObjectName("sectionPromote");remove=new QPushButton(tr("Delete"),body);remove->setObjectName("sectionDelete");auto save=new QPushButton(tr("Save"),body);save->setObjectName("sectionSave");persistence->addWidget(promote);persistence->addWidget(remove);persistence->addWidget(save);layout->addLayout(persistence);
  status=new QLabel(body);status->setObjectName("sectionFeedback");status->setWordWrap(true);layout->addWidget(status);layout->addStretch();
  auto scroll=new QScrollArea(this);scroll->setWidgetResizable(true);scroll->setWidget(body);setWidget(scroll);
  connect(controller,&Controller::changed,this,&Dock::refresh);
  connect(global,&QCheckBox::toggled,this,[this](bool value){if(!updating)this->controller->setGlobalEnabled(value);});
  connect(helper,&QCheckBox::toggled,this,[this](bool value){if(!updating)this->controller->setHelperEnabled(this->controller->selectedId(),value);});
  connect(rows,&QTreeWidget::currentItemChanged,this,[this](auto *item,auto *){if(!updating && item)this->controller->select(item->data(0,Qt::UserRole).toString().toStdString());});
  connect(rows,&QTreeWidget::itemChanged,this,[this](auto *item,int column){if(!updating && column==0){this->controller->enable(item->data(0,Qt::UserRole).toString().toStdString(),item->checkState(0)==Qt::Checked);refresh();}});
  connect(name,&QLineEdit::editingFinished,this,[this]{if(updating)return;Design::FieldChange change{Design::ChangeKind::Label};change.label=name->text().toStdString();this->controller->edit(this->controller->selectedId(),{change});});
  connect(offset,&QDoubleSpinBox::valueChanged,this,[this](double value){if(updating)return;if(!this->controller->beginGesture(this->controller->selectedId()))return;Design::FieldChange change{Design::ChangeKind::Offset};change.offset=value;this->controller->preview({change});});
  connect(offset,&QDoubleSpinBox::editingFinished,this,[this]{if(!updating)this->controller->finishGesture();});
  connect(slider,&QSlider::sliderPressed,this,[this]{if(!updating)this->controller->beginGesture(this->controller->selectedId());});
  connect(slider,&QSlider::valueChanged,this,[this](int value){if(updating)return;auto range=this->controller->offsetRange(this->controller->selectedId());if(!range)return;Design::FieldChange change{Design::ChangeKind::Offset};change.offset=range->first+(range->second-range->first)*value/1000.;if(slider->isSliderDown())this->controller->preview({change});else this->controller->edit(this->controller->selectedId(),{change});});
  connect(slider,&QSlider::sliderReleased,this,[this]{this->controller->finishGesture();});
  connect(azimuth,&QDoubleSpinBox::editingFinished,this,&Dock::commitAngles);connect(elevation,&QDoubleSpinBox::editingFinished,this,&Dock::commitAngles);
  connect(pickPoint,&QPushButton::clicked,this,[this]{this->controller->pick(this->controller->selectedId(),false);});connect(pickAlign,&QPushButton::clicked,this,[this]{this->controller->pick(this->controller->selectedId(),true);});
  connect(flip,&QPushButton::clicked,this,[this]{this->controller->flip(this->controller->selectedId());});connect(promote,&QPushButton::clicked,this,[this]{this->controller->promote(this->controller->selectedId());});connect(remove,&QPushButton::clicked,this,[this]{this->controller->remove(this->controller->selectedId());});
  connect(save,&QPushButton::clicked,this,[parent]{if(auto *window=qobject_cast<MainWindow *>(parent))window->tabManager->save(window->activeEditor);});
}
void Dock::commitAngles(){if(!updating)controller->setAngles(controller->selectedId(),azimuth->value(),elevation->value());}
void Dock::refresh() {
  if(updating)return;updating=true;auto all=controller->records();rows->clear();
  for(const auto& [id,record]:all)if(record->kind=="Section"){QString diagnostic;bool resolved=controller->frame(id,&diagnostic).has_value();auto item=new QTreeWidgetItem(rows,{QString::fromStdString(record->label),record->source?tr("Source"):tr("Transient"),resolved?tr("Ready"):diagnostic});item->setData(0,Qt::UserRole,QString::fromStdString(id));item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,controller->enabled(id)?Qt::Checked:Qt::Unchecked);if(controller->selectedId()==id)rows->setCurrentItem(item);}
  for(const auto& id:controller->enabledSections())if(!all.count(id)){auto item=new QTreeWidgetItem(rows,{QString::fromStdString(id),tr("Unavailable"),tr("Missing or stale Section")});item->setData(0,Qt::UserRole,QString::fromStdString(id));item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,Qt::Checked);}
  global->setChecked(controller->globalEnabled());auto selected=all.find(controller->selectedId());QString reason;bool editable=controller->editable(controller->selectedId(),&reason);
  name->setEnabled(editable);offset->setEnabled(editable);slider->setEnabled(editable);azimuth->setEnabled(editable);elevation->setEnabled(editable);pickPoint->setEnabled(editable);pickAlign->setEnabled(editable);flip->setEnabled(editable);helper->setEnabled(selected!=all.end());helper->setChecked(controller->helperEnabled(controller->selectedId()));remove->setEnabled(selected!=all.end());promote->setEnabled(selected!=all.end() && !selected->second->source);
  for(auto spin:origin)spin->setEnabled(editable);
  name->setText(selected==all.end()?QString():QString::fromStdString(selected->second->label));
  if(selected!=all.end())if(auto section=std::dynamic_pointer_cast<const Design::Section>(selected->second->evaluated)){
    offset->setValue(section->offset);auto range=controller->offsetRange(selected->first);slider->setEnabled(editable && range && range->second>range->first);if(range && range->second>range->first){slider->setValue(qRound(std::clamp((section->offset-range->first)/(range->second-range->first),0.,1.)*1000));offset->setSingleStep((range->second-range->first)/100);}
    if(auto pose=controller->frame(selected->first)){for(int axis=0;axis<3;++axis)origin[axis]->setValue(pose->translation()[axis]);Vector3d normal=pose->linear().col(2);double degrees=180/std::acos(-1.);if(normal.head<2>().norm()>1e-10)azimuth->setValue(std::atan2(normal.y(),normal.x())*degrees);elevation->setValue(std::asin(std::clamp(normal.z(),-1.,1.))*degrees);}}
  status->setText(editable?controller->feedback():reason+"\n"+controller->feedback());updating=false;
}
}
