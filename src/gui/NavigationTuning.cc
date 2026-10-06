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
#include <QStyle>
#include <QToolButton>
#include <QWhatsThis>

NavigationTuning::NavigationTuning(QGLView *view) : Dock(view->window()),view(view)
{
  setObjectName("navigationTuning");
  setName(tr("Navigation tuning"));
  resize(390,620);
  auto content=new QWidget(this);
  setWidget(content);
  auto layout=new QVBoxLayout(content);
  auto developmentNotice=new QLabel(tr("Development only: this panel is solely for fine-tuning software parameters. "
                                      "It will not be included in any production release."),content);
  developmentNotice->setWordWrap(true);
  layout->addWidget(developmentNotice);
  auto hint=new QLabel(tr("Live session overrides. Copy settings to retain an experiment."),content);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto scroll=new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto body=new QWidget(scroll);
  auto form=new QFormLayout(body);
  auto settings=&view->navigation()->settings;
  const QMap<QString,QString> helpText{
    {"scale",tr("Overall size of the cube, arrows and menu triangle. 1 means the base size; higher values enlarge them. Default: 1.1.")},
    {"cubeRight",tr("Distance from the navigation widget's right edge to the viewport's right edge, in logical pixels. Higher values move it left.")},
    {"cubeTop",tr("Distance from the navigation widget's top edge to the viewport's top edge, in logical pixels. Higher values move it down.")},
    {"menuRight",tr("Distance from the menu triangle's button to the viewport's right edge, in logical pixels. Higher values move it left, independently of the cube.")},
    {"menuTop",tr("Distance from the menu triangle's button to the viewport's top edge, in logical pixels. Higher values move it down, independently of the cube.")},
    {"chamfer",tr("Fraction of each cube edge used for the bevel. Higher values make the edge and corner targets larger and the flat faces smaller.")},
    {"cubeRadius",tr("Size of the cube geometry in logical pixels before overall scaling. Changes the cube itself without moving the surrounding buttons.")},
    {"labelSize",tr("Cube face label font size in logical pixels before overall scaling. Higher values make labels larger.")},
    {"arrowSize",tr("Size of the direction and roll buttons in logical pixels before overall scaling. Changes both the symbols and their clickable areas.")},
    {"arrowInset",tr("Moves the four direction arrows toward the cube, in logical pixels before overall scaling. 0 keeps the original spacing; higher values move them closer. Large values can overlap the cube.")},
    {"rollInset",tr("Moves the two curved roll buttons toward the cube, in logical pixels before overall scaling. 0 keeps the original spacing; higher values move them closer. Large values can overlap other controls.")},
    {"idleOpacity",tr("Opacity of the navigation controls when inactive. 0 is invisible; 1 is fully opaque.")},
    {"activeOpacity",tr("Opacity while hovering, using the controls, focusing them with the keyboard, or opening the viewport menu. 0 is invisible; 1 is fully opaque.")},
    {"enterDelay",tr("Wait in milliseconds before fading to active opacity after entering or activating the navigation controls. 0 starts immediately.")},
    {"leaveDelay",tr("Wait in milliseconds before fading back to idle opacity after leaving the controls. Higher values keep them visible longer.")},
    {"fadeIn",tr("Time in milliseconds to transition to active opacity after the enter delay. 0 changes opacity immediately.")},
    {"fadeOut",tr("Time in milliseconds to transition to idle opacity after the leave delay. 0 changes opacity immediately.")},
    {"visible",tr("Show the navigation cube and its viewport-menu triangle. Turn this back on here if you hide them.")},
    {"animate",tr("Animate camera changes such as snapping to a cube target or recalling a saved view. When off, the camera changes immediately.")},
    {"freeOrbit",tr("Allow dragging the cube to orbit the camera freely. When off, cube target clicks and the arrow buttons still work.")},
    {"duration",tr("Camera transition duration in milliseconds when Animate is on. Higher values make transitions slower; 0 makes them immediate.")},
    {"easing",tr("Speed profile for animated camera transitions. Linear uses constant progress; Ease in/out and Smooth sine start and finish gently.")},
    {"rollStep",tr("Rotation around the viewing direction, in degrees per curved-arrow click or Q/E key press while the cube has focus. Default: 15 degrees.")},
    {"arrowStep",tr("Rotation in degrees per direction-arrow click or arrow-key press while the cube has focus. Default: 15 degrees.")},
    {"dragThreshold",tr("Pointer movement required before a cube press becomes a drag, in logical pixels (horizontal plus vertical movement). Higher values tolerate more hand movement during a click. Initially follows the system drag threshold.")},
    {"orbitSensitivity",tr("Multiplier for camera rotation when dragging the cube with Free orbit enabled. 1 is normal; lower values rotate less, higher values rotate more.")},
    {"showInformation",tr("Master switch for the viewport information overlay. Triangles, FPS and Frame time are shown only when this is on.")},
    {"triangles",tr("Show the renderer's triangle count when Show information is on. The overlay identifies preview or final geometry and reports unavailable or stale counts.")},
    {"fps",tr("Show viewport frames per second when Show information is on. Based on recent completed-frame intervals; displays Idle when frames stop.")},
    {"frameTime",tr("Show the average interval between completed viewport frames in milliseconds when Show information is on. This measures frame spacing, not CPU or GPU render time alone.")},
    {"statsRefresh",tr("Minimum interval in milliseconds between statistics text updates while frames are arriving. Higher values make the readout update less often.")},
    {"statsWindow",tr("Maximum number of recent frame intervals used to average FPS and frame time. Higher values smooth fluctuations but respond more slowly.")},
    {"statsIdle",tr("Time in milliseconds without a completed frame before the readout switches to Idle and clears its averaging history.")}
  };
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
      else if (name.endsWith("Inset")) {
        spin->setRange(0,25);
        spin->setSingleStep(1);
      }
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
    const QString help=helpText.value(it.key());
    control->setToolTip(help);
    control->setWhatsThis(help);
    control->setAccessibleDescription(help);
    auto labelRow=new QWidget(body);
    auto labelLayout=new QHBoxLayout(labelRow);
    labelLayout->setContentsMargins(0,0,0,0);
    labelLayout->setSpacing(4);
    auto caption=new QLabel(label,labelRow);
    caption->setBuddy(control);
    labelLayout->addWidget(caption);
    auto helpButton=new QToolButton(labelRow);
    helpButton->setObjectName("help_"+it.key());
    helpButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarContextHelpButton));
    helpButton->setAutoRaise(true);
    helpButton->setAccessibleName(tr("Help for %1").arg(label));
    helpButton->setAccessibleDescription(help);
    helpButton->setToolTip(help);
    connect(helpButton,&QToolButton::clicked,this,[helpButton,help] {
      QWhatsThis::showText(helpButton->mapToGlobal(QPoint(0,helpButton->height())),help,helpButton);
    });
    labelLayout->addWidget(helpButton);
    labelLayout->addStretch();
    form->addRow(labelRow,control);
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
