#include "gui/clipping/ClippingDock.h"
#include "gui/clipping/ClippingController.h"
#include "gui/MainWindow.h"
#include "gui/TabManager.h"
#include <QTreeWidget>
#include <QComboBox>
#include <QToolButton>
#include <QStyle>
#include <QToolTip>
#include <QTimer>
#include <QEvent>
#include <QFile>
#include <QPainter>
#include <QSvgRenderer>
#include <array>
#include <QCheckBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QInputDialog>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QLocale>
#include <limits>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QSplitter>
#include <QHeaderView>

namespace Clipping {
namespace {
class SectionList : public QTreeWidget {
public:
  explicit SectionList(QWidget *parent) : QTreeWidget(parent) { setUniformRowHeights(true); }
  QSize sizeHint() const override {
    const int rowHeight=topLevelItemCount()?sizeHintForRow(0):fontMetrics().height()+4;
    return QSize(QTreeWidget::sizeHint().width(),header()->sizeHint().height()+5*rowHeight+2*frameWidth());
  }
};
// Use the Customizer's help artwork and tooltip interaction in standalone builds too.
class HelpButton : public QToolButton {
public:
  explicit HelpButton(QWidget *parent) : QToolButton(parent) {
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    timer.setSingleShot(true);timer.setInterval(250);
    connect(&timer,&QTimer::timeout,this,[this]{showHelp();});
    connect(this,&QToolButton::clicked,this,[this]{showHelp();});
  }
  QSize sizeHint() const override {
    const int size=style()->pixelMetric(QStyle::PM_SmallIconSize,nullptr,this);
    return QSize(size+4,size+4);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    static const QByteArray artwork=[] {
      QFile file(":/icons/tabler/stock/help-circle.svg");file.open(QIODevice::ReadOnly);return file.readAll();
    }();
    auto svg=artwork;const auto color=palette().color(isEnabled()?QPalette::Active:QPalette::Disabled,QPalette::WindowText);
    svg.replace("currentColor",color.name().toUtf8());
    QSvgRenderer renderer(svg);QPainter painter(this);painter.setOpacity(color.alphaF());
    const int size=style()->pixelMetric(QStyle::PM_SmallIconSize,nullptr,this);
    renderer.render(&painter,QRectF((width()-size)/2.,(height()-size)/2.,size,size));
  }
  bool event(QEvent *event) override {
    switch(event->type()) {
    case QEvent::Enter: timer.start();break;
    case QEvent::Leave:
    case QEvent::Hide: timer.stop();QToolTip::hideText();break;
    case QEvent::ToolTip: showHelp();return true;
    default: break;
    }
    return QToolButton::event(event);
  }
private:
  QTimer timer;
  void showHelp() {
    timer.stop();
    if(isVisible())QToolTip::showText(mapToGlobal(rect().bottomLeft()),toolTip(),this,rect());
  }
};
const std::array<Vector3d,6> cutDirections={Vector3d(0,0,1),Vector3d(0,0,-1),
  Vector3d(0,1,0),Vector3d(0,-1,0),Vector3d(1,0,0),Vector3d(-1,0,0)};
}
Dock::Dock(Controller *controller,QWidget *parent):QDockWidget(tr("Cross sections"),parent),controller(controller) {
  setObjectName("clippingDock");auto container=new QWidget(this);auto outer=new QVBoxLayout(container);
  global=new QCheckBox(tr("Enable cross section clipping"),container);global->setObjectName("sectionGlobal");outer->addWidget(global);
  auto lifecycle=new QHBoxLayout;
  auto create=new QPushButton(tr("New cross section"),container);create->setObjectName("sectionNew");lifecycle->addWidget(create);
  remove=new QPushButton(tr("Delete"),container);remove->setObjectName("sectionDelete");lifecycle->addWidget(remove);
  outer->addLayout(lifecycle);
  connect(create,&QPushButton::clicked,this,[this]{this->controller->createSection();});
  auto splitter=new QSplitter(Qt::Vertical,container);splitter->setObjectName("sectionListSplitter");splitter->setChildrenCollapsible(false);
  outer->addWidget(splitter,1);
  rows=new SectionList(splitter);rows->setObjectName("sectionRows");rows->setHeaderLabels({tr("Cross section"),tr("Storage")});rows->setRootIsDecorated(false);
  splitter->addWidget(rows);
  auto body=new QWidget;auto layout=new QVBoxLayout(body);layout->setContentsMargins(0,0,0,0);
  auto form=new QFormLayout;
  directionPreset=new QComboBox(body);directionPreset->setObjectName("sectionDirectionPreset");
  directionPreset->addItems({tr("Above (+Z)"),tr("Below (−Z)"),tr("Toward +Y"),tr("Toward −Y"),tr("Toward +X"),tr("Toward −X"),tr("Custom")});
  directionPreset->setToolTip(tr("The vector points toward the side that is removed. The cutting plane is perpendicular to it."));
  form->addRow(tr("Cut away"),directionPreset);
  for(int axis=0;axis<3;++axis){
    origin[axis]=new QDoubleSpinBox(body);origin[axis]->setObjectName("sectionOrigin"+QString("XYZ").mid(axis,1));
    origin[axis]->setDecimals(8);origin[axis]->setRange(-1e100,1e100);origin[axis]->setKeyboardTracking(false);
    originLabel[axis]=new QLabel(body);originLabel[axis]->setBuddy(origin[axis]);
    form->addRow(originLabel[axis],origin[axis]);
    // Steps apply immediately; typed text is committed on Enter/focus loss.
    connect(origin[axis],qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,axis](double value){
      if(updating)return;
      const auto id=this->controller->selectedId();auto pose=this->controller->frame(id);if(!pose)return;
      Vector3d point=pose->translation();point[axis]=value;
      this->controller->setOrigin(id,point);
    });
  }
  directionFields=new QWidget(body);directionFields->setObjectName("sectionDirectionFields");
  auto directionForm=new QFormLayout(directionFields);
  directionForm->setContentsMargins(16,0,0,0);
  form->addRow(directionFields);directionFields->hide();
  for(int axis=0;axis<3;++axis){
    direction[axis]=new QLineEdit(directionFields);direction[axis]->setObjectName("sectionDirection"+QString("XYZ").mid(axis,1));
    direction[axis]->setLocale(QLocale::c());
    auto validator=new QDoubleValidator(-std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),1000,direction[axis]);
    validator->setLocale(QLocale::c());validator->setNotation(QDoubleValidator::ScientificNotation);direction[axis]->setValidator(validator);
    direction[axis]->setToolTip(tr("Finite vector component; scientific notation is accepted. The vector must not be zero."));
    directionForm->addRow(tr("Direction %1").arg(QString("XYZ").mid(axis,1)),direction[axis]);
    connect(direction[axis],&QLineEdit::editingFinished,this,&Dock::commitDirection);
  }
  connect(directionPreset,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){
    if(updating)return;
    updateCoordinateVisibility();
    const auto id=this->controller->selectedId();
    if(index==6){customDirectionId=id;for(auto spin:direction)spin->setEnabled(this->controller->editable(id));}
    else if(index>=0 && index<int(cutDirections.size())){customDirectionId.clear();this->controller->setCutAwayDirection(id,cutDirections[index]);}
  });
  layout->addLayout(form);
  updateCoordinateVisibility();
  helper=new QCheckBox(tr("Show cutting plane"),body);helper->setObjectName("sectionHelper");layout->addWidget(helper);
  auto originGroup=new QGroupBox(tr("Set cutting plane origin"),body);originGroup->setObjectName("sectionOriginGroup");
  auto picking=new QHBoxLayout(originGroup);
  pickPoint=new QPushButton(tr("Pick point"),originGroup);pickPoint->setObjectName("sectionPickPoint");picking->addWidget(pickPoint);
  pickAlign=new QPushButton(tr("Pick + align"),originGroup);pickAlign->setObjectName("sectionPickAlign");picking->addWidget(pickAlign);
  layout->addWidget(originGroup);
  auto actions=new QHBoxLayout;flip=new QPushButton(tr("Flip"),body);flip->setObjectName("sectionFlip");actions->addWidget(flip);actions->addStretch();layout->addLayout(actions);
  auto persistence=new QHBoxLayout;promote=new QPushButton(tr("Save cross section to source"),body);promote->setObjectName("sectionPromote");persistence->addWidget(promote);
  auto help=new HelpButton(body);help->setObjectName("sectionHelp");
  help->setAccessibleName(tr("Cross section help"));
  const auto helpText=tr("New cross sections are temporary for this session. Use Save cross section to source, then the normal file Save command, to keep them after reopening. "
       "The shaded rectangle shows the cutting plane; it does not fill the cut surface. The dot marks its origin. "
       "The Cut away vector points toward the side that is removed. The cutting plane is perpendicular to it. "
       "Axis presets show only the coordinate that moves the cut. Custom shows the three origin coordinates and direction components. "
       "New origins start at world coordinates [0, 0, 0]. Pick point copies a surface point into the origin; Pick + align also copies its direction. "
       "Picking does not attach the cross section to the model. Custom vectors are normalized automatically and must not be zero.");
  help->setToolTip("<p>"+helpText.toHtmlEscaped()+"</p>");help->setAccessibleDescription(helpText);
  persistence->addWidget(help);layout->addLayout(persistence);
  status=new QLabel(body);status->setObjectName("sectionFeedback");status->setWordWrap(true);layout->addWidget(status);layout->addStretch();
  auto scroll=new QScrollArea(splitter);scroll->setObjectName("sectionDetails");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(body);
  splitter->addWidget(scroll);splitter->setStretchFactor(0,0);splitter->setStretchFactor(1,1);
  splitter->setSizes({rows->sizeHint().height(),400});
  splitter->handle(1)->setAccessibleName(tr("Resize cross section list"));
  splitter->handle(1)->setToolTip(tr("Drag to resize the cross section list"));
  setWidget(container);
  connect(controller,&Controller::changed,this,&Dock::refresh);
  connect(global,&QCheckBox::toggled,this,[this](bool value){if(!updating)this->controller->setGlobalEnabled(value);});
  connect(helper,&QCheckBox::toggled,this,[this](bool value){if(!updating)this->controller->setHelperEnabled(this->controller->selectedId(),value);});
  connect(rows,&QTreeWidget::currentItemChanged,this,[this](auto *item,auto *){if(!updating && item)this->controller->select(item->data(0,Qt::UserRole).toString().toStdString());});
  rows->setToolTip(tr("Double-click a cross section's name to rename it."));
  connect(rows,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem *item,int column){
    if(updating || !item || column!=0)return;
    // Refresh replaces list items, including while a modal dialog is open. Retain identities only.
    const auto id=item->data(0,Qt::UserRole).toString().toStdString();
    const auto documentId=this->controller->documentId();
    const auto oldName=item->text(0);
    if(!this->controller->editable(id))return;
    QInputDialog dialog(this);dialog.setObjectName("sectionRenameDialog");
    dialog.setWindowTitle(tr("Rename cross section"));dialog.setLabelText(tr("Name"));dialog.setTextValue(oldName);
    if(dialog.exec()!=QDialog::Accepted || this->controller->documentId()!=documentId)return;
    const auto newName=dialog.textValue().trimmed();
    if(newName.isEmpty() || newName==oldName)return;
    Design::FieldChange change{Design::ChangeKind::Label};change.label=newName.toStdString();
    this->controller->edit(id,{change});
  });
  connect(rows,&QTreeWidget::itemChanged,this,[this](auto *item,int column){if(!updating && column==0){this->controller->enable(item->data(0,Qt::UserRole).toString().toStdString(),item->checkState(0)==Qt::Checked);refresh();}});
  connect(pickPoint,&QPushButton::clicked,this,[this]{this->controller->pick(this->controller->selectedId(),false);});connect(pickAlign,&QPushButton::clicked,this,[this]{this->controller->pick(this->controller->selectedId(),true);});
  connect(flip,&QPushButton::clicked,this,[this]{this->controller->flip(this->controller->selectedId());});connect(promote,&QPushButton::clicked,this,[this]{this->controller->promote(this->controller->selectedId());});connect(remove,&QPushButton::clicked,this,[this]{this->controller->remove(this->controller->selectedId());});
}
void Dock::commitDirection(){
  if(updating || directionPreset->currentIndex()!=6)return;
  customDirectionId=controller->selectedId();
  Vector3d entered;
  for(int axis=0;axis<3;++axis){bool valid=false;entered[axis]=QLocale::c().toDouble(direction[axis]->text(),&valid);if(!valid)entered[axis]=std::numeric_limits<double>::quiet_NaN();}
  if(!entered.allFinite() || !(entered.cwiseAbs().maxCoeff()>0)){
    // Reject invalid input here, before it can become a cross section edit.
    for(int axis=0;axis<3;++axis)direction[axis]->setText(QString::number(displayedCutAway[axis],'g',17));
    QToolTip::showText(directionFields->mapToGlobal(directionFields->rect().bottomLeft()),
      tr("Enter a finite, nonzero direction. The previous direction has been kept."),directionFields);
    return;
  }
  committingDirection=true;
  controller->setCutAwayDirection(customDirectionId,entered);
  committingDirection=false;
}
void Dock::updateCoordinateVisibility() {
  const int preset=directionPreset->currentIndex();
  const bool custom=preset==6;
  const int axis=preset<2?2:preset<4?1:0;
  for(int i=0;i<3;++i){
    const bool visible=custom || i==axis;
    origin[i]->setVisible(visible);originLabel[i]->setVisible(visible);
    originLabel[i]->setText((custom?tr("Origin %1"):tr("Position %1")).arg(QString("XYZ").mid(i,1)));
  }
  directionFields->setVisible(custom);
}
void Dock::refresh() {
  if(updating)return;updating=true;auto all=controller->displayRecords();rows->clear();
  for(const auto& [id,record]:all)if(record->kind=="Section"){
    auto item=new QTreeWidgetItem(rows,{QString::fromStdString(record->label),record->source?tr("Source"):tr("Session only")});
    item->setData(0,Qt::UserRole,QString::fromStdString(id));item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,controller->enabled(id)?Qt::Checked:Qt::Unchecked);if(controller->selectedId()==id)rows->setCurrentItem(item);
  }
  for(const auto& id:controller->enabledSections())if(!all.count(id)){auto item=new QTreeWidgetItem(rows,{QString::fromStdString(id),tr("Missing definition")});item->setToolTip(0,tr("Saved cross section has no current definition. Remove this entry, or evaluate its source."));item->setData(0,Qt::UserRole,QString::fromStdString(id));item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,Qt::Checked);if(controller->selectedId()==id)rows->setCurrentItem(item);}
  global->setChecked(controller->globalEnabled());auto selected=all.find(controller->selectedId());QString reason;bool editable=controller->editable(controller->selectedId(),&reason);
  directionPreset->setEnabled(editable);pickPoint->setEnabled(editable);pickAlign->setEnabled(editable);flip->setEnabled(editable);helper->setEnabled(selected!=all.end());helper->setChecked(controller->helperEnabled(controller->selectedId()));remove->setEnabled(selected!=all.end() || controller->enabled(controller->selectedId()));remove->setText(selected!=all.end()?tr("Delete"):tr("Remove entry"));promote->setEnabled(selected!=all.end() && !selected->second->source);
  const bool canPick=editable && controller->evaluationCurrent() && !controller->bounds().isEmpty();
  if(selected!=all.end() && selected->second->source && !editable)remove->setEnabled(false);
  pickPoint->setEnabled(canPick);pickAlign->setEnabled(canPick);
  const auto pickTip=canPick?tr("Copy a point from the model without attaching the cross section."):tr("Evaluate a model with geometry to pick a surface point.");
  pickPoint->setToolTip(pickTip);pickAlign->setToolTip(pickTip);
  promote->setEnabled(selected!=all.end() && !selected->second->source && controller->sourceStateCurrent());
  promote->setToolTip(controller->sourceStateCurrent()?QString():tr("Preview the document before saving the cross section to source."));

  for(auto spin:origin)spin->setEnabled(editable);
  if(directionDocumentId!=controller->documentId() || directionSelectionId!=controller->selectedId()){
    customDirectionId.clear();directionDocumentId=controller->documentId();directionSelectionId=controller->selectedId();
    displayedCutAway=Vector3d::Zero();directionPreset->setCurrentIndex(0);
    for(auto component:direction)component->clear();
    for(auto component:origin)component->setValue(0);
  }
  if(selected!=all.end())if(auto pose=controller->frame(selected->first)){
    for(int axis=0;axis<3;++axis)origin[axis]->setValue(pose->translation()[axis]);
    const Vector3d cutAway=-pose->linear().col(2);
    // Keep the entered magnitude during our own edits. Flip,
    // source edits and undo may replace the pose, so refresh their direction normally.
    if(!committingDirection && !cutAway.isApprox(displayedCutAway,1e-8))customDirectionId.clear();
    int preset=6;
    if(customDirectionId!=selected->first)for(int i=0;i<int(cutDirections.size());++i)if(cutAway.isApprox(cutDirections[i],1e-8)){preset=i;break;}
    directionPreset->setCurrentIndex(preset);
    if(customDirectionId!=selected->first)for(int axis=0;axis<3;++axis)direction[axis]->setText(QString::number(cutAway[axis],'g',17));
    displayedCutAway=cutAway;
  }
  for(auto spin:direction)spin->setEnabled(editable && directionPreset->currentIndex()==6);
  updateCoordinateVisibility();
  status->setText((editable?controller->feedback():reason+"\n"+controller->feedback()).trimmed());status->setVisible(!status->text().isEmpty());updating=false;
}
}
