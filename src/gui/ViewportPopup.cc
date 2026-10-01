#include "gui/ViewportPopup.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/NavigationCube.h"
#include "gui/QSettingsCached.h"
#include "gui/SavedViews.h"
#include "gui/TabManager.h"
#include <QLineEdit>
#include <QTimer>
#include <QStyledItemDelegate>
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
class ViewNameDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void setModelData(QWidget *editor,QAbstractItemModel *model,const QModelIndex& index) const override {
    const auto line=qobject_cast<QLineEdit *>(editor);
    if (line && line->text().trimmed().isEmpty()) return;
    QStyledItemDelegate::setModelData(editor,model,index);
  }
};
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
  auto projectionAvailability=[this] {
    perspective->setEnabled(this->window->viewActionPerspective->isEnabled());
    orthographic->setEnabled(this->window->viewActionOrthogonal->isEnabled());
  };
  connect(window->viewActionPerspective,&QAction::changed,this,projectionAvailability);
  connect(window->viewActionOrthogonal,&QAction::changed,this,projectionAvailability);
  auto commands=new QHBoxLayout;
  const QList<QAction *> actions{window->viewActionCenter,window->viewActionViewAll,window->viewActionResetView};
  const QStringList names{tr("Center"),tr("View All"),tr("Reset")};
  for (int i=0;i<3;++i) {
    auto button=new QPushButton(names[i],content);
    commands->addWidget(button);
    connect(button,&QPushButton::clicked,actions[i],&QAction::trigger);
    auto action=actions[i];
    button->setEnabled(action->isEnabled());
    connect(action,&QAction::changed,button,[button,action]{button->setEnabled(action->isEnabled());});
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
    return box;
  };
  auto information=settingBox(tr("Show information"),"showInformation");
  layout->addWidget(information);
  auto informationItems=new QVBoxLayout;
  informationItems->setContentsMargins(20,0,0,0);
  layout->addLayout(informationItems);
  for (const auto& item : QList<QPair<QString,QByteArray>>{{tr("Triangles"),"triangles"},{tr("FPS"),"fps"},{tr("Frame time"),"frameTime"}}) {
    auto box=settingBox(item.first,item.second);
    informationItems->addWidget(box);
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
    box->setEnabled(action->isEnabled());
    display->addWidget(box,i%2,i/2);
    connect(box,&QCheckBox::clicked,action,&QAction::trigger);
    connect(action,&QAction::changed,box,[box,action] {
      const QSignalBlocker block(box); box->setChecked(action->isChecked()); box->setEnabled(action->isEnabled());
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
  views->setEditTriggers(QAbstractItemView::EditKeyPressed);
  views->setItemDelegate(new ViewNameDelegate(views));
  layout->addWidget(views,1);
  auto buttons=new QHBoxLayout;
  addView=new QPushButton(tr("Add"),content);
  removeView=new QPushButton(tr("Remove"),content);
  renameView=new QPushButton(tr("Rename"),content);
  for (auto button : {addView,removeView,renameView}) { buttons->addWidget(button); button->setEnabled(false); }
  layout->addLayout(buttons);
  layout->addWidget(new QLabel(tr("Add stores the current camera."),content));
  connect(addView,&QPushButton::clicked,this,[this] {
    if (!store) return;
    const auto id=store->add(this->window->qglview->navigation()->current());
    for (int i=0;i<views->count();++i) if (views->item(i)->data(Qt::UserRole).toString()==id) {
      views->setCurrentRow(i); views->editItem(views->item(i)); break;
    }
  });
  connect(removeView,&QPushButton::clicked,this,[this] {
    if (store && views->currentItem()) store->remove(views->currentItem()->data(Qt::UserRole).toString());
  });
  connect(renameView,&QPushButton::clicked,this,[this] {
    if (views->currentItem()) views->editItem(views->currentItem());
  });
  connect(views,&QListWidget::currentRowChanged,this,[this] {
    removeView->setEnabled(store && views->currentItem());
    renameView->setEnabled(store && views->currentItem());
  });
  connect(views,&QListWidget::itemChanged,this,[this](QListWidgetItem *item) {
    if (!store) return;
    const QString id=item->data(Qt::UserRole).toString();
    store->rename(id,item->text());
  });
  connect(views,&QListWidget::itemActivated,this,[this]{recallView();});
  connect(window,&MainWindow::displayedDocumentChanged,this,&ViewportPopup::synchronize);
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
  const QSignalBlocker a(perspective),b(orthographic);
  perspective->setChecked(!ortho); orthographic->setChecked(ortho);
  perspective->setEnabled(window->viewActionPerspective->isEnabled());
  orthographic->setEnabled(window->viewActionOrthogonal->isEnabled());
  window->viewActionPerspective->setChecked(!ortho);
  window->viewActionOrthogonal->setChecked(ortho);
  const auto editor=window->displayedDocument();
  documentLabel->setText(editor ? (editor->filepath.isEmpty() ? tr("Untitled") : QFileInfo(editor->filepath).fileName()) : tr("No displayed document"));
  const auto owner=editor ? SavedViews::forEditor(editor) : nullptr;
  if (store!=owner) {
    if (store) disconnect(store,nullptr,this,nullptr);
    store=owner;
    if (store) connect(store,&SavedViews::changed,this,&ViewportPopup::refreshViews);
    refreshViews();
  }
  addView->setEnabled(store && store->problem().isEmpty() && store->entries().size()<1000);
  views->setToolTip(store ? store->problem() : tr("Compile a document to associate saved views with the viewport."));
}

void ViewportPopup::refreshViews()
{
  const QSignalBlocker block(views);
  // Update by identity so deferred name commits do not destroy a subsequent inline editor.
  for (int row=views->count()-1;row>=0;--row) {
    if (!store || !store->find(views->item(row)->data(Qt::UserRole).toString())) delete views->takeItem(row);
  }
  if (store) for (const auto& entry:store->entries()) {
    QListWidgetItem *item=nullptr;
    for (int row=0;row<views->count();++row) if (views->item(row)->data(Qt::UserRole).toString()==entry.id) {item=views->item(row);break;}
    if (!item) {
      item=new QListWidgetItem(entry.name,views);
      item->setData(Qt::UserRole,entry.id);
      item->setFlags(item->flags()|Qt::ItemIsEditable);
    } else if (item->text()!=entry.name) item->setText(entry.name);
  }
  removeView->setEnabled(store && views->currentItem());
  renameView->setEnabled(store && views->currentItem());
}
void ViewportPopup::recallView()
{
  if (!store || !views->currentItem()) return;
  const auto editor=views->findChild<QLineEdit *>();
  if (editor && editor->isVisible()) return;
  const auto entry=store->find(views->currentItem()->data(Qt::UserRole).toString());
  if (entry) window->qglview->navigation()->navigate(entry->camera);
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
