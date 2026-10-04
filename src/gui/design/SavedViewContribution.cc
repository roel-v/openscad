#include "gui/design/SavedViewContribution.h"
#include "gui/design/DesignDocument.h"
#include "gui/MainWindow.h"
#include "gui/Editor.h"
#include "gui/SavedViews.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/selection/EvaluationBinding.h"
#include "core/design/DesignReferences.h"
#include "core/Tree.h"
#include "geometry/GeometryEvaluator.h"
#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace Design {
SavedViewContribution::SavedViewContribution(MainWindow *window):QObject(window),window(window){
  setObjectName("savedViewContribution");
  contribution=window->inspectorDock->registerContribution({"savedViews",30,this,[this](const Selection::State&,const Selection::Stamp& stamp){return build(stamp);}});
}
SavedViewContribution *SavedViewContribution::forWindow(MainWindow& window){
  for(auto *child:window.children())if(auto *service=dynamic_cast<SavedViewContribution *>(child))return service;
  return new SavedViewContribution(&window);
}
QWidget *SavedViewContribution::build(const Selection::Stamp& stamp){
  auto *body=new QWidget;body->setObjectName("savedViewContributionBody");auto *layout=new QVBoxLayout(body);
  auto *title=new QLabel(tr("Saved views"),body);layout->addWidget(title);
  auto *list=new QComboBox(body);list->setObjectName("designViewList");list->setMinimumContentsLength(20);list->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);layout->addWidget(list);
  auto *owner=window->activeEditor;
  if(!owner){layout->addWidget(new QLabel(tr("No document is open."),body));return body;}
  auto *personal=SavedViews::forEditor(owner);auto *document=DesignDocument::forEditor(*owner,*window);
  for(auto *object:{static_cast<QObject *>(personal),static_cast<QObject *>(document)})if(!watched.contains(object)){
    watched.insert(object);connect(object,&QObject::destroyed,this,[this,object]{watched.remove(object);});
    if(object==personal)connect(personal,&SavedViews::changed,this,[this]{if(window)window->inspectorDock->refresh();});
    else connect(document,&DesignDocument::changed,this,[this]{if(window)window->inspectorDock->refresh();});
  }
  auto current=document->records(stamp);
  for(const auto& [id,record]:current)if(record->kind=="View")list->addItem(QString::fromStdString(record->label)+(record->source?tr(" · Source"):tr(" · Transient")),QStringList{QString::fromStdString(id),record->source?"source":"transient"});
  for(const auto& entry:personal->entries())list->addItem(entry.name()+tr(" · Personal inspection"),QStringList{entry.id(),"personal"});
  auto *status=new QLabel(window->evaluationBinding->validate(stamp)?tr("Source views follow the current evaluated model."):tr("Source views require a current evaluation."),body);status->setWordWrap(true);layout->addWidget(status);
  QPointer<SavedViewContribution> service(this);QPointer<EditorInterface> editor(owner);QPointer<DesignDocument> doc(document);QPointer<SavedViews> store(personal);QPointer<QComboBox> chooser(list);
  auto action=[&](const QString& text,const QString& name,const std::function<void(QString,QString)>& callback){
    auto *button=new QPushButton(text,body);button->setObjectName(name);button->setEnabled(list->count()>0);layout->addWidget(button);
    connect(button,&QPushButton::clicked,this,[service,editor,chooser,callback]{
      if(!service || !editor || !chooser)return;auto selected=chooser->currentData().toStringList();if(selected.size()!=2)return;
      QTimer::singleShot(0,service,[service,editor,selected,callback]{if(service && editor && service->window && service->window->activeEditor==editor)callback(selected[0],selected[1]);});
    });
  };
  auto report=[service](const std::string& message){if(service && service->window && !message.empty())QMessageBox::warning(service->window,tr("Saved view"),QString::fromStdString(message));};
  action(tr("Apply"),"applyDesignView",[service,doc,store,stamp,report](QString id,QString backing){
    if(!service || !service->window || !service->window->qglview->navigation()->permitted())return;
    if(backing=="personal"){auto *entry=store?store->find(id):nullptr;if(entry)service->window->qglview->navigation()->apply(entry->pose());return;}
    auto records=doc?doc->records(stamp):Records{};auto found=records.find(id.toStdString());if(found==records.end()){report("View evaluation became stale");return;}
    auto scene=service->window->evaluationBinding->scene(stamp);Tree tree(scene->root,scene->documentPath);GeometryEvaluator evaluator(tree);auto geometry=evaluator.evaluateGeometry(*scene->root,false);
    ResolveContext context{stamp.resultId.toStdString(),stamp.sourceFingerprint.toStdString(),stamp.configurationFingerprint.toStdString(),geometry?geometry->getBoundingBox():BoundingBox(),records,{}};
    std::string error;auto camera=viewCamera(*found->second,context,&error);if(!camera){report(error);return;}
    if(service && service->window && service->window->evaluationBinding->validate(stamp))service->window->qglview->navigation()->apply(CameraPose::from(*camera));
  });
  action(tr("Promote to source"),"promoteDesignView",[doc,editor,stamp,report](QString id,QString backing){
    if(!doc || !editor)return;if(backing=="source")return;
    auto result=backing=="personal"?doc->promotePersonal(id.toStdString(),*editor,stamp):doc->promote({id.toStdString()},*editor,stamp);report(result.error);
  });
  action(tr("Rename"),"renameDesignView",[service,doc,store,editor,stamp,report](QString id,QString backing){
    if(!service || !service->window || !editor)return;bool accepted=false;auto name=QInputDialog::getText(service->window,tr("Rename view"),tr("Name"),QLineEdit::Normal,{},&accepted);if(!accepted || name.trimmed().isEmpty() || !editor || !service || service->window->activeEditor!=editor)return;
    if(backing=="personal"){if(store)store->rename(id,name);return;}
    if(!doc)return;auto token=doc->beginEdit(id.toStdString(),stamp);if(!token){report("View evaluation became stale");return;}
    FieldChange change{ChangeKind::Label};change.label=name.toStdString();auto updated=doc->updateDraft(*token,{change});if(!updated){report(updated.error);return;}report(doc->commitDraft(*token).error);
  });
  action(tr("Delete"),"deleteDesignView",[doc,store,stamp,report](QString id,QString backing){if(backing=="personal"){if(store)store->remove(id);}else if(doc)report(doc->remove(id.toStdString(),stamp).error);});
  return body;
}
}
