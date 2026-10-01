#include "gui/ViewportPopup.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/NavigationCube.h"
#include "gui/QSettingsCached.h"
#include <QVBoxLayout>
#include <QGridLayout>
#include <QRadioButton>
#include <QCheckBox>
#include <QLabel>
#include <QSizeGrip>
#include <QScrollArea>
#include <QApplication>
#include <QScreen>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QFileInfo>

namespace {
class CompactViewList : public QListWidget {
public:
  using QListWidget::QListWidget;
  QSize sizeHint() const override { return QSize(240,80); }
  QSize minimumSizeHint() const override { return QSize(120,60); }
};
}

ViewportPopup::ViewportPopup(MainWindow *window)
  : QFrame(window->qglview,Qt::Popup | Qt::FramelessWindowHint),window(window)
{
  setObjectName("viewportPopup");
  setAccessibleName(tr("Viewport settings"));
  setAttribute(Qt::WA_NoMouseReplay);
  setFrameStyle(QFrame::Box | QFrame::Plain);
  setLineWidth(1);
  setMinimumSize(230,260);
  auto outer=new QVBoxLayout(this);
  outer->setContentsMargins(5,5,5,3);
  auto scroll=new QScrollArea(this);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  auto content=new QWidget(scroll);
  auto layout=new QVBoxLayout(content);
  layout->setContentsMargins(5,5,5,5);
  layout->setSpacing(4);
  auto heading=[&](const QString& text) {
    auto label=new QLabel(text,content);
    auto font=label->font(); font.setBold(true); label->setFont(font);
    layout->addWidget(label);
  };
  heading(tr("Projection"));
  auto projectionRow=new QHBoxLayout;
  perspective=new QRadioButton(tr("Perspective"),content);
  orthographic=new QRadioButton(tr("Orthographic"),content);
  perspective->setObjectName("viewportPerspective");
  orthographic->setObjectName("viewportOrthographic");
  projectionRow->addWidget(perspective); projectionRow->addWidget(orthographic);
  layout->addLayout(projectionRow);
  connect(perspective,&QRadioButton::clicked,window->viewActionPerspective,&QAction::trigger);
  connect(orthographic,&QRadioButton::clicked,window->viewActionOrthogonal,&QAction::trigger);
  auto commands=new QHBoxLayout;
  const QList<QAction *> actions{window->viewActionCenter,window->viewActionViewAll,window->viewActionResetView};
  const QStringList names{tr("Center"),tr("View All"),tr("Reset")};
  for (int i=0;i<3;++i) {
    auto button=new QPushButton(names[i],content);
    commands->addWidget(button);
    connect(button,&QPushButton::clicked,actions[i],&QAction::trigger);
  }
  layout->addLayout(commands);
  heading(tr("Viewport information"));
  auto settings=&window->qglview->navigation()->settings;
  auto settingBox=[&](const QString& text,const QByteArray& property) {
    auto box=new QCheckBox(text,content);
    box->setObjectName("viewport_"+QString::fromLatin1(property));
    box->setChecked(settings->property(property).toBool());
    connect(box,&QCheckBox::toggled,this,[property](bool value){ NavigationSettings::setPersistent(property,value); });
    connect(settings,&NavigationSettings::changed,box,[box,settings,property] {
      const QSignalBlocker block(box);
      box->setChecked(settings->property(property).toBool());
    });
    layout->addWidget(box);
    return box;
  };
  auto information=settingBox(tr("Show information"),"showInformation");
  for (const auto& item : QList<QPair<QString,QByteArray>>{{tr("Triangles"),"triangles"},{tr("FPS"),"fps"},{tr("Frame time"),"frameTime"}}) {
    auto box=settingBox(item.first,item.second);
    box->setEnabled(information->isChecked());
    connect(information,&QCheckBox::toggled,box,&QWidget::setEnabled);
    connect(settings,&NavigationSettings::changed,box,[box,settings]{box->setEnabled(settings->showInformation);});
  }
  heading(tr("Display"));
  auto display=new QGridLayout;
  const QList<QAction *> displayActions{window->viewActionShowAxes,window->viewActionShowScaleProportional,
                                        window->viewActionShowCrosshairs,window->viewActionShowEdges};
  const QStringList displayNames{tr("Axes"),tr("Scale markers"),tr("Crosshairs"),tr("Edges")};
  for (int i=0;i<4;++i) {
    auto box=new QCheckBox(displayNames[i],content);
    auto action=displayActions[i];
    box->setChecked(action->isChecked());
    display->addWidget(box,i%2,i/2);
    connect(box,&QCheckBox::clicked,action,&QAction::trigger);
    connect(action,&QAction::changed,box,[box,action] {
      const QSignalBlocker block(box); box->setChecked(action->isChecked());
    });
  }
  layout->addLayout(display);
  heading(tr("Saved views"));
  documentLabel=new QLabel(tr("No displayed document"),content);
  documentLabel->setWordWrap(true);
  layout->addWidget(documentLabel);
  views=new CompactViewList(content);
  views->setObjectName("savedViews");
  views->setAccessibleName(tr("Saved camera views"));
  views->setMinimumHeight(60);
  layout->addWidget(views,1);
  auto buttons=new QHBoxLayout;
  addView=new QPushButton(tr("Add"),content);
  removeView=new QPushButton(tr("Remove"),content);
  renameView=new QPushButton(tr("Rename"),content);
  for (auto button : {addView,removeView,renameView}) { buttons->addWidget(button); button->setEnabled(false); }
  layout->addLayout(buttons);
  layout->addWidget(new QLabel(tr("Add stores the current camera."),content));
  scroll->setWidget(content);
  outer->addWidget(scroll,1);
  outer->addWidget(new QSizeGrip(this),0,Qt::AlignRight);
  connect(window->qglview->navigation(),&CameraNavigation::displayChanged,this,&ViewportPopup::synchronize);
  connect(settings,&NavigationSettings::changed,this,[this,settings] { if (!settings->visible) hide(); });
  connect(window->qglview->navigationCube(),&NavigationCube::menuRequested,this,&ViewportPopup::toggle);
  synchronize();
}

void ViewportPopup::synchronize()
{
  const bool ortho=window->qglview->orthoMode();
  const QSignalBlocker a(perspective),b(orthographic),c(window->viewActionPerspective),d(window->viewActionOrthogonal);
  perspective->setChecked(!ortho); orthographic->setChecked(ortho);
  window->viewActionPerspective->setChecked(!ortho);
  window->viewActionOrthogonal->setChecked(ortho);
  const auto editor=window->displayedDocument();
  documentLabel->setText(editor ? (editor->filepath.isEmpty() ? tr("Untitled") : QFileInfo(editor->filepath).fileName()) : tr("No displayed document"));
}

void ViewportPopup::toggle()
{
  if (isVisible()) { hide(); return; }
  previousFocus=QApplication::focusWidget();
  synchronize();
  resize(QSettingsCached().value("navigation/popupSize",QSize(300,480)).toSize().expandedTo(minimumSize()));
  const auto triangle=window->qglview->navigationCube()->menuTriangle();
  const auto anchor=triangle->mapToGlobal(QPoint(triangle->width(),triangle->height()));
  const auto screen=QGuiApplication::screenAt(anchor);
  const QRect bounds=(screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
  resize(size().boundedTo(bounds.size()));
  const int preferredX=window->qglview->navigationCube()->mapToGlobal(QPoint(0,0)).x()-width()-8;
  move(qBound(bounds.left(),preferredX,bounds.right()-width()+1),
       qBound(bounds.top(),anchor.y()+4,bounds.bottom()-height()+1));
  show(); raise();
  perspective->setFocus(Qt::PopupFocusReason);
  window->qglview->navigationCube()->setPopupActive(true);
}

void ViewportPopup::hideEvent(QHideEvent *event)
{
  QSettingsCached().setValue("navigation/popupSize",size());
  window->qglview->navigationCube()->setPopupActive(false);
  if (previousFocus) previousFocus->setFocus(Qt::PopupFocusReason);
  QFrame::hideEvent(event);
}

void ViewportPopup::keyPressEvent(QKeyEvent *event)
{
  if (event->key()==Qt::Key_Escape) { hide(); event->accept(); return; }
  QFrame::keyPressEvent(event);
}
