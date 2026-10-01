#include "gui/NavigationTuning.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/NavigationCube.h"
#include "gui/QSettingsCached.h"
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QLabel>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QSignalBlocker>

NavigationTuning::NavigationTuning(QGLView *view) : Dock(view->window()),view(view)
{
  setObjectName("navigationTuning");
  setName(tr("Navigation tuning"));
  resize(390,620);
  auto content=new QWidget(this);
  setWidget(content);
  auto layout=new QVBoxLayout(content);
  auto hint=new QLabel(tr("Live session overrides. Copy settings to retain an experiment."),content);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto scroll=new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto body=new QWidget(scroll);
  auto form=new QFormLayout(body);
  auto settings=&view->navigation()->settings;
  const auto values=settings->values();
  for (auto it=values.cbegin();it!=values.cend();++it) {
    const auto name=it.key().toLatin1();
    QString label=it.key();
    label.replace(QRegularExpression("([a-z])([A-Z])"),"\\1 \\2");
    label[0]=label[0].toUpper();
    QWidget *control=nullptr;
    if (name=="easing") {
      auto combo=new QComboBox(body);
      combo->addItem(tr("Linear"),QEasingCurve::Linear);
      combo->addItem(tr("Ease in/out"),QEasingCurve::InOutCubic);
      combo->addItem(tr("Smooth sine"),QEasingCurve::InOutSine);
      combo->setCurrentIndex(combo->findData(settings->easing));
      connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,[settings,combo,name] {
        settings->setTemporary(name,combo->currentData());
      });
      connect(settings,&NavigationSettings::changed,combo,[settings,combo] {
        const QSignalBlocker block(combo); combo->setCurrentIndex(combo->findData(settings->easing));
      });
      control=combo;
    } else if (it.value().userType()==QMetaType::Bool) {
      auto box=new QCheckBox(body); box->setChecked(it.value().toBool());
      connect(box,&QCheckBox::toggled,this,[settings,name](bool value){settings->setTemporary(name,value);});
      connect(settings,&NavigationSettings::changed,box,[settings,box,name] {
        const QSignalBlocker block(box); box->setChecked(settings->property(name).toBool());
      });
      control=box;
    } else {
      auto spin=new QDoubleSpinBox(body);
      const bool integer=it.value().userType()==QMetaType::Int;
      spin->setDecimals(integer ? 0 : 2);
      spin->setRange(0,2000);
      spin->setSingleStep(integer ? 10 : 1);
      if (name=="scale") { spin->setRange(0.35,3); spin->setSingleStep(0.05); }
      else if (name=="chamfer") { spin->setRange(0.08,0.45); spin->setSingleStep(0.01); }
      else if (name.endsWith("Opacity")) { spin->setRange(0,1); spin->setSingleStep(0.05); }
      else if (name=="cubeRadius") spin->setRange(20,45);
      else if (name=="labelSize") spin->setRange(7,24);
      else if (name=="arrowSize") spin->setRange(12,30);
      else if (name=="dragThreshold") spin->setRange(1,32);
      else if (name=="orbitSensitivity") { spin->setRange(0.1,3); spin->setSingleStep(0.1); }
      else if (name.endsWith("Step")) spin->setRange(1,180);
      else if (name=="duration") spin->setRange(0,5000);
      else if (name=="statsWindow") spin->setRange(2,240);
      else if (name=="statsRefresh") spin->setRange(50,2000);
      else if (name=="statsIdle") spin->setRange(100,10000);
      spin->setSuffix(" "+NavigationSettings::unit(name));
      spin->setValue(it.value().toDouble());
      connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[settings,name,integer](double value) {
        settings->setTemporary(name,integer ? QVariant(qRound(value)) : QVariant(value));
      });
      connect(settings,&NavigationSettings::changed,spin,[settings,spin,name] {
        const QSignalBlocker block(spin); spin->setValue(settings->property(name).toDouble());
      });
      control=spin;
    }
    control->setObjectName("tune_"+it.key());
    control->setAccessibleName(label);
    form->addRow(label,control);
  }
  auto position=new QLabel(body);
  position->setWordWrap(true);
  auto updatePosition=[this,position] {
    const auto cube=this->view->navigationCube();
    const auto triangle=cube->menuTriangle();
    position->setText(tr("Resolved positions (logical px): cube %1, %2; triangle %3, %4\nViewport %5 × %6; DPR %7")
      .arg(cube->x()).arg(cube->y()).arg(triangle->x()).arg(triangle->y())
      .arg(this->view->width()).arg(this->view->height()).arg(this->view->devicePixelRatioF()));
  };
  form->addRow(position);
  connect(settings,&NavigationSettings::changed,position,updatePosition);
  connect(view,&QGLView::resized,position,updatePosition);
  updatePosition();
  scroll->setWidget(body); layout->addWidget(scroll,1);
  auto buttons=new QHBoxLayout;
  auto copy=new QPushButton(tr("Copy settings"),this);
  copy->setObjectName("copyNavigationSettings");
  auto reset=new QPushButton(tr("Reset overrides"),this);
  buttons->addWidget(copy); buttons->addWidget(reset);
  auto feedback=new QLabel(this);
  layout->addLayout(buttons); layout->addWidget(feedback);
  connect(copy,&QPushButton::clicked,this,[this,feedback] {
    QApplication::clipboard()->setText(description()); feedback->setText(tr("Copied"));
  });
  connect(reset,&QPushButton::clicked,settings,&NavigationSettings::resetOverrides);
}

QString NavigationTuning::description() const
{
  QString text=view->navigation()->settings.description();
  auto rect=[](const QRect& r) {return QString("x=%1 y=%2 width=%3 height=%4 logical px").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());};
  const auto cube=view->navigationCube();
  text+="anchor = viewport upper-right\n";
  text+="cube resolved = "+rect(cube->geometry())+"\n";
  text+="triangle resolved = "+rect(cube->menuTriangle()->geometry())+"\n";
  text+=QString("viewport = %1 × %2 logical px; DPR = %3\n").arg(view->width()).arg(view->height()).arg(view->devicePixelRatioF());
  text+=QString("projection = %1; axes = %2; scale markers = %3; crosshairs = %4; edges = %5\n")
    .arg(view->orthoMode() ? "orthographic" : "perspective").arg(view->showAxes()).arg(view->showScaleProportional())
    .arg(view->showCrosshairs()).arg(view->showEdges());
  const auto size=QSettingsCached().value("navigation/popupSize",QSize(300,480)).toSize();
  if (auto popup=view->findChild<QWidget *>("viewportPopup")) text+="popup current (screen coordinates) = "+rect(popup->geometry())+"\n";
  text+=QString("saved popup size = %1 × %2 logical px\n").arg(size.width()).arg(size.height());
  return text;
}
