#include "TestDesignObjects.h"
#include "gui/design/DesignDocument.h"
#include "gui/design/DesignEdits.h"
#include "gui/source/EditorUndoRegistry.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/InspectorDock.h"
#include "gui/ScintillaEditor.h"
#include "gui/SavedViews.h"
#include "gui/CameraNavigation.h"
#include "gui/QGLView.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignReferences.h"
#include "core/Tree.h"
#include "geometry/GeometryEvaluator.h"
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <sstream>

namespace {
QByteArray sourceSection(const QByteArray& offset="0"){
  return "height=40;\r\n@{type: Section}\r\nmiddle={id:\"middle\",label:\"Middle\",origin:[0,0,height/2],normal:[0,0,1],up:[0,1,0],adjust:{offset:"+offset+",rotation:[0,0,0,1]}};\r\ncube([20,20,height]);\r\n";
}
bool write(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(bytes)==bytes.size();}
QByteArray read(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
bool compile(MainWindow& window){
  QSignalSpy completed(&window,&MainWindow::compilationDone);window.actionRenderPreview();
  QElapsedTimer timer;timer.start();while(completed.empty() && timer.elapsed()<15000)QTest::qWait(10);
  return !completed.empty() && window.evaluationBinding->current();
}
Value vector(const Vector3d& value){VectorType result(nullptr);for(int i=0;i<3;++i)result.emplace_back(Value(value[i]));return Value(std::move(result));}
Value definition(const std::string& id,const std::string& kind="Section",const std::string& dependency=""){
  ObjectType object(nullptr);object.set("id",Value(id));object.set("label",Value(id));
  if(kind=="Section"){object.set("origin",vector(Vector3d(0,0,20)));object.set("normal",vector(Vector3d::UnitZ()));}
  else{object.set("direction",vector(Vector3d::UnitY()));object.set("up",vector(Vector3d::UnitZ()));}
  if(!dependency.empty()){ObjectType reference(nullptr);reference.set("domain",Value("design"));reference.set("id",Value(dependency));object.set("frame",Value(std::move(reference)));}
  return Value(std::move(object));
}
std::shared_ptr<const Design::Record> copy(const Design::Record& record,bool sourced=true){
  auto result=std::make_shared<Design::Record>();result->id=record.id;result->kind=record.kind;result->label=record.label;result->definition=Design::ownedValue(record.definition);result->evaluated=record.evaluated;
  if(sourced)result->source=record.source;for(const auto& token:record.dependencies)result->dependencies.push_back({token.domain,token.id,Design::ownedValue(token.data)});return result;
}
AgentAnnotations::Record annotation(){
  QImage image(8,8,QImage::Format_RGB32);image.fill(Qt::blue);QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");
  return {{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"comment","design lifecycle"},{"created_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    {"width",8},{"height",8},{"png_sha256",QString::fromLatin1(AgentAnnotations::Store::hash(png))},
    {"camera",QJsonObject{{"translation",QJsonArray{0,0,0}},{"rotation",QJsonArray{0,0,0}},{"distance",100},{"fov",45},{"projection","perspective"}}},{"strokes",QJsonArray{}},{"result",QJsonObject{}}},png};
}
struct CancelDialogs {
  QTimer timer;bool seen=false;
  CancelDialogs(){timer.setInterval(10);QObject::connect(&timer,&QTimer::timeout,[this]{
    if(auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())){auto *button=box->button(QMessageBox::Cancel);if(!button)button=box->button(QMessageBox::Ok);if(button){seen=true;button->click();}}
  });timer.start();}
};
}
void TestDesignObjects::nativeDeclarationsAndParametricEdits(){
  window->designActionAutoReload->setChecked(false);window->hide();window->show();window->raise();window->activateWindow();QVERIFY(QTest::qWaitForWindowExposed(window));
  window->tabManager->actionNew();auto *editor=window->activeEditor;editor->setPlainText(QString::fromUtf8(sourceSection()));QVERIFY(compile(*window));
  auto document=Design::DesignDocument::forEditor(*editor,*window);auto stamp=window->evaluationBinding->stamp();auto records=document->records(stamp);QVERIFY(records.count("middle"));QVERIFY(records.at("middle")->source);
  auto draft=document->beginEdit("middle",stamp);QVERIFY(draft);Design::FieldChange offset{Design::ChangeKind::Offset};offset.offset=5;
  Design::FieldChange rotate{Design::ChangeKind::Rotation};rotate.rotation=Eigen::Quaterniond(Eigen::AngleAxisd(std::acos(-1.),Vector3d::UnitY()));
  QVERIFY(document->updateDraft(*draft,{offset,rotate}));QVERIFY(document->commitDraft(*draft));
  QVERIFY(editor->toPlainText().contains("height/2"));QVERIFY(editor->toPlainText().contains("offset:5") || editor->toPlainText().contains("offset: 5"));QVERIFY(compile(*window));
  stamp=window->evaluationBinding->stamp();records=document->records(stamp);auto payload=std::dynamic_pointer_cast<const Design::Section>(records.at("middle")->evaluated);QCOMPARE(payload->origin.z(),20.);QCOMPARE(payload->offset,5.);
  editor->undo();QCOMPARE(editor->toPlainText().toUtf8(),sourceSection());editor->redo();QVERIFY(editor->toPlainText().contains("height/2"));
  QTemporaryDir directory;auto path=directory.filePath("parametric.scad");QVERIFY(window->tabManager->saveAs(editor,path));
  window->tabManager->closeCurrentTab();window->tabManager->open(path);editor=window->activeEditor;QVERIFY(compile(*window));
  auto bytes=editor->toPlainText().toUtf8();auto index=bytes.indexOf("40");SourceEditing::EditRequest change{editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{{index,index+2,"40","60"}}};
  QCOMPARE(SourceEditing::apply(*editor,change).error,SourceEditing::EditError::None);QVERIFY(compile(*window));
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());QCOMPARE(std::dynamic_pointer_cast<const Design::Section>(scene->designObjects->records.at("middle")->evaluated)->origin.z(),30.);
  QVERIFY(window->tabManager->save(editor));window->tabManager->closeCurrentTab();
}
void TestDesignObjects::promotionUndoRedoIdentityAndDelete(){
  window->tabManager->actionNew();auto *editor=dynamic_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(editor);editor->setPlainText("cube(10);\n");editor->clearUndoHistory();
  editor->qsci->setCursorPosition(1,0);QTest::keyClicks(editor->qsci,"// typing");auto typed=editor->toPlainText();QVERIFY(editor->annotations->add(annotation()));QVERIFY(compile(*window));
  auto document=Design::DesignDocument::forEditor(*editor,*window);QVERIFY(document->createTransient("Section",definition("shared")));QVERIFY(document->createTransient("View",definition("consumer","View","shared")));
  auto stamp=window->evaluationBinding->stamp();auto promoted=document->promote({"consumer"},*editor,stamp);QVERIFY2(promoted,qPrintable(QString::fromStdString(promoted.error)));
  auto saved=editor->toPlainText();QVERIFY(saved.indexOf("section_shared")<saved.indexOf("view_consumer"));QVERIFY(document->transientRecords().empty());
  editor->undo();QCOMPARE(editor->toPlainText(),typed);QCOMPARE(document->transientRecords().size(),size_t(2));
  editor->undo();QVERIFY(editor->annotations->records().isEmpty());editor->undo();QCOMPARE(editor->toPlainText(),QString("cube(10);\n"));
  editor->redo();QCOMPARE(editor->toPlainText(),typed);editor->redo();QCOMPARE(editor->annotations->records().size(),1);editor->redo();QCOMPARE(editor->toPlainText(),saved);QVERIFY(document->transientRecords().empty());
  QVERIFY(compile(*window));stamp=window->evaluationBinding->stamp();auto draft=document->beginEdit("shared",stamp);QVERIFY(draft);Design::FieldChange label{Design::ChangeKind::Label};label.label="Renamed";
  QVERIFY(document->updateDraft(*draft,{label}));QVERIFY(document->commitDraft(*draft));QVERIFY(compile(*window));stamp=window->evaluationBinding->stamp();QCOMPARE(document->records(stamp).at("shared")->label,std::string("Renamed"));
  QVERIFY(document->remove("shared",stamp));QVERIFY(compile(*window));stamp=window->evaluationBinding->stamp();QVERIFY(!document->records(stamp).count("shared"));
  auto records=document->records(stamp);Design::ResolveContext context{"r","i","c",BoundingBox(Vector3d::Zero(),Vector3d(10,10,10)),records,{}};
  QCOMPARE(Design::resolve({"design","shared",Value::undefined.clone()},context).state,Design::ResolutionState::Missing);
  editor->undo();QVERIFY(compile(*window));QVERIFY(document->records(window->evaluationBinding->stamp()).count("shared"));
  stamp=window->evaluationBinding->stamp();QVERIFY(document->createTransient("Section",definition("temporary")));auto temporary=document->beginEdit("temporary",stamp);QVERIFY(temporary);
  Design::FieldChange offset{Design::ChangeKind::Offset};offset.offset=-7;auto sourceBefore=editor->toPlainText();QVERIFY(document->updateDraft(*temporary,{offset}));QVERIFY(document->commitDraft(*temporary));
  QCOMPARE(std::dynamic_pointer_cast<const Design::Section>(document->transientRecords().at("temporary")->evaluated)->offset,-7.);editor->undo();QCOMPARE(editor->toPlainText(),sourceBefore);
  QCOMPARE(std::dynamic_pointer_cast<const Design::Section>(document->transientRecords().at("temporary")->evaluated)->offset,0.);editor->redo();QVERIFY(document->remove("temporary",stamp));QVERIFY(!document->transientRecords().count("temporary"));editor->undo();QVERIFY(document->transientRecords().count("temporary"));QCOMPARE(editor->toPlainText(),sourceBefore);
  QTemporaryDir directory;QVERIFY(window->tabManager->saveAs(editor,directory.filePath("interleaved.scad")));window->tabManager->closeCurrentTab();
}
void TestDesignObjects::transactionFailureHistoryAndLifetime(){
  window->tabManager->actionNew();auto *editor=dynamic_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(editor);editor->setPlainText("cube(1);");editor->clearUndoHistory();
  auto registry=SourceEditing::EditorUndoRegistry::forEditor(*editor);int state=0;auto lifetime=new QObject(editor);auto participant=std::make_shared<SourceEditing::UndoParticipant>();participant->lifetime=lifetime;participant->undo=[&]{state=0;};participant->redo=[&]{state=1;};
  auto original=editor->toPlainText();SourceEditing::EditRequest request{editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{{5,6,"wrong","2"}},participant};
  QCOMPARE(SourceEditing::apply(*editor,request).error,SourceEditing::EditError::ExpectedTextMismatch);QCOMPARE(registry->size(),size_t(0));QCOMPARE(state,0);QCOMPARE(editor->toPlainText(),original);QVERIFY(!editor->canUndo());
  request.edits[0].expected="1";QCOMPARE(SourceEditing::apply(*editor,request).error,SourceEditing::EditError::None);QCOMPARE(registry->size(),size_t(1));QCOMPARE(state,1);
  editor->undo();QCOMPARE(state,0);QCOMPARE(editor->toPlainText(),original);editor->redo();QCOMPARE(state,1);
  std::weak_ptr<const SourceEditing::UndoParticipant> weak=participant;request.participant.reset();participant.reset();QVERIFY(!weak.expired());editor->clearUndoHistory();QVERIFY(weak.expired());QCOMPARE(registry->size(),size_t(0));
  auto expired=std::make_shared<SourceEditing::UndoParticipant>();expired->lifetime=lifetime;expired->undo=[]{};expired->redo=[]{};delete lifetime;
  request={editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{},expired};QCOMPARE(SourceEditing::apply(*editor,request).error,SourceEditing::EditError::InvalidParticipant);
  auto retained=std::make_shared<SourceEditing::UndoParticipant>();retained->lifetime=editor;retained->undo=[]{};retained->redo=[]{};
  request={editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{},retained};QCOMPARE(SourceEditing::apply(*editor,request).error,SourceEditing::EditError::None);
  std::weak_ptr<const SourceEditing::UndoParticipant> closed=retained;request.participant.reset();retained.reset();QVERIFY(!closed.expired());
  editor->setContentModified(false);QPointer<EditorInterface> guard(editor);window->tabManager->closeCurrentTab();QTRY_VERIFY(guard.isNull());QVERIFY(closed.expired());
}
void TestDesignObjects::dirtyIncludeGuardsAndSourceOnlyFields(){
  QTemporaryDir directory;auto root=directory.filePath("root.scad"),library=directory.filePath("include.scad");QVERIFY(write(root,"include <include.scad> cube(height);"));QVERIFY(write(library,sourceSection("height/4")));
  window->tabManager->open(library);auto *included=dynamic_cast<ScintillaEditor *>(window->activeEditor);QVERIFY(included);window->tabManager->open(root);auto *owner=window->activeEditor;QVERIFY(compile(*window));
  Design::DesignEdits edits(window);auto stamp=window->evaluationBinding->stamp();Design::FieldChange offset{Design::ChangeKind::Offset};offset.offset=3;
  auto computed=edits.prepare("middle",{offset},stamp);QVERIFY(!computed);QVERIFY(QString::fromStdString(computed.error).contains("expression"));
  Design::FieldChange label{Design::ChangeKind::Label};label.label="New";auto prepared=edits.prepare("middle",{label},stamp);QVERIFY(prepared);QVERIFY(prepared.editor==included);
  included->qsci->setReadOnly(true);QCOMPARE(SourceEditing::apply(*included,prepared.request).error,SourceEditing::EditError::ReadOnly);included->qsci->setReadOnly(false);
  included->insertOrReplaceText("// dirty\n");QCOMPARE(SourceEditing::apply(*included,prepared.request).error,SourceEditing::EditError::StaleSource);
  QVERIFY(!edits.prepare("middle",{label},stamp));QVERIFY(owner==window->activeEditor);
  included->setContentModified(false);window->tabManager->open(library);window->tabManager->closeCurrentTab();window->tabManager->open(root);window->tabManager->closeCurrentTab();
}
void TestDesignObjects::scopedLifecycleAdapters(){
  struct State { Design::Records records; int rebound=0; };
  auto state=std::make_shared<State>();std::weak_ptr<State> weak=state;
  QVERIFY(Design::registerKind({"NativeLifecycle",{Design::DeclarationHost::Assignment},{Design::DeclarationScope::File,Design::DeclarationScope::Local},Design::decodeSection,Design::RecordDestination::Consumer,
    [weak](Design::Record record,const Design::EvaluatedDeclaration& declaration,EvaluationSession&){if(auto state=weak.lock())state->records.insert_or_assign(declaration.source.hostName,copy(record));}}));
  window->tabManager->actionNew();auto *editor=window->activeEditor;editor->setPlainText("cube(1);\n");QVERIFY(compile(*window));
  auto document=Design::DesignDocument::forEditor(*editor,*window);auto lifetime=new QObject(document);
  auto handler=Design::kind("NativeLifecycle");std::vector<Design::Diagnostic> diagnostics;
  QVERIFY(document->createTransient("Section",definition("required-section")));
  for(const std::string key:{"owner_one","owner_two"}){
    auto record=Design::makeRecord(*handler,definition("same-local-id","Section","required-section"),{{"","",key},Design::DeclarationScope::File},diagnostics);QVERIFY(record);state->records.emplace(key,copy(*record,false));
  }
  Design::LifecycleCallbacks callbacks;
  callbacks.resolveConsumerRecord=[weak](const std::string& key,const Selection::Stamp&){auto state=weak.lock();if(!state)return std::shared_ptr<const Design::Record>();auto found=state->records.find(key);return found==state->records.end()?nullptr:found->second;};
  callbacks.prepareInsert=[editor](const Design::Draft& draft,const Selection::Stamp&){
    auto bytes=editor->toPlainText().toUtf8();std::ostringstream text;text<<"@{type: NativeLifecycle}\n"<<draft.key<<" = "<<draft.before->definition<<";\n";
    return Design::PreparedEdit{editor,{editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{{bytes.size(),bytes.size(),{},QByteArray::fromStdString(text.str())}}},{}};
  };
  callbacks.prepareEdit=[](const std::string&,const std::vector<Design::FieldChange>&,const Selection::Stamp&){return Design::PreparedEdit{{},{},"Test consumer supports creation only"};};
  callbacks.locatePromotedRecord=[weak](const Design::Draft& draft,const Selection::Stamp&){auto state=weak.lock();if(!state)return std::shared_ptr<const Design::Record>();++state->rebound;auto found=state->records.find(draft.key);return found!=state->records.end() && found->second->source?found->second:nullptr;};
  auto registration=document->registerLifecycleAdapter("NativeLifecycle",lifetime,callbacks);QVERIFY(registration);QVERIFY(!document->registerLifecycleAdapter("NativeLifecycle",lifetime,callbacks));
  auto stamp=window->evaluationBinding->stamp();auto one=document->beginConsumerEdit("NativeLifecycle","owner_one",stamp),two=document->beginConsumerEdit("NativeLifecycle","owner_two",stamp);QVERIFY(one && two && one!=two);
  QCOMPARE(document->consumerRecord("NativeLifecycle","owner_one",stamp)->id,std::string("same-local-id"));QCOMPARE(document->consumerRecord("NativeLifecycle","owner_two",stamp)->id,std::string("same-local-id"));
  auto promoted=document->promoteConsumer(*one);QVERIFY2(promoted,qPrintable(QString::fromStdString(promoted.error)));auto after=editor->toPlainText();QVERIFY(after.indexOf("section_required_section")<after.indexOf("owner_one ="));QVERIFY(document->transientRecords().empty());
  editor->undo();QCOMPARE(editor->toPlainText(),QString("cube(1);\n"));QVERIFY(document->drafts().count(*one));QVERIFY(document->transientRecords().count("required-section"));editor->redo();QCOMPARE(editor->toPlainText(),after);QVERIFY(document->transientRecords().empty());
  QVERIFY(compile(*window));QVERIFY(state->rebound>0);auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());QCOMPARE(scene->designObjects->records.size(),size_t(1));QVERIFY(scene->designObjects->records.count("required-section"));
  stamp=window->evaluationBinding->stamp();two=document->beginConsumerEdit("NativeLifecycle","owner_two",stamp);QVERIFY(two);
  editor->insertOrReplaceText("// stale\n");QVERIFY(!document->promoteConsumer(*two));QVERIFY(!editor->toPlainText().contains("owner_two ="));
  QVERIFY(compile(*window));stamp=window->evaluationBinding->stamp();two=document->beginConsumerEdit("NativeLifecycle","owner_two",stamp);QVERIFY(two);delete lifetime;QVERIFY(document->drafts().empty());QVERIFY(!document->consumerRecord("NativeLifecycle","owner_one",stamp));
  editor->setContentModified(false);window->tabManager->closeCurrentTab();
}
void TestDesignObjects::consumerTransientDraftValidationAndJointUndo(){
  struct State {std::map<std::string,std::shared_ptr<const Design::Record>> records;};
  auto state=std::make_shared<State>();std::weak_ptr<State> weak=state;
  QVERIFY(Design::registerKind({"NativeDraft",{Design::DeclarationHost::Assignment},{Design::DeclarationScope::File},Design::decodeSection,Design::RecordDestination::Consumer,
    [weak](Design::Record record,const Design::EvaluatedDeclaration& declaration,EvaluationSession&){if(auto state=weak.lock())state->records.insert_or_assign(declaration.source.hostName,copy(record));}}));
  window->tabManager->actionNew();auto *editor=window->activeEditor;editor->setPlainText("cube(1);\n");editor->clearUndoHistory();QVERIFY(compile(*window));
  auto document=Design::DesignDocument::forEditor(*editor,*window);auto lifetime=new QObject(document);
  Design::LifecycleCallbacks callbacks;
  callbacks.resolveConsumerRecord=[weak](const std::string& key,const Selection::Stamp&){auto state=weak.lock();if(!state)return std::shared_ptr<const Design::Record>();auto found=state->records.find(key);return found==state->records.end()?nullptr:found->second;};
  callbacks.prepareInsert=[editor](const Design::Draft& draft,const Selection::Stamp&){auto bytes=editor->toPlainText().toUtf8();std::ostringstream text;text<<"@{type:NativeDraft} "<<draft.key<<"="<<draft.preview->definition<<";\n";return Design::PreparedEdit{editor,{editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{{bytes.size(),bytes.size(),{},QByteArray::fromStdString(text.str())}}},{}};};
  callbacks.prepareEdit=[](const auto&,const auto&,const auto&){return Design::PreparedEdit{{},{},"Creation fixture"};};
  callbacks.locatePromotedRecord=[weak](const Design::Draft& draft,const Selection::Stamp&){auto state=weak.lock();if(!state)return std::shared_ptr<const Design::Record>();auto found=state->records.find(draft.key);return found==state->records.end()?nullptr:found->second;};
  callbacks.previewConsumerRecord=[](const Design::Record& before,const std::vector<Design::FieldChange>& changes,std::string& error)->std::shared_ptr<const Design::Record>{
    auto object=before.definition.toObject().clone();ObjectType adjustment(nullptr);
    for(const auto& change:changes){double value=0;if(change.kind!=Design::ChangeKind::LiteralField || change.fieldPath!="adjust.offset" || !change.literal || !change.literal->getFiniteDouble(value)){error="Fixture requires one finite offset literal";return {}; }adjustment.set("offset",Value(value));}
    object.set("adjust",Value(std::move(adjustment)));std::vector<Design::Diagnostic> diagnostics;auto decoded=Design::makeRecord(*Design::kind("NativeDraft"),Value(std::move(object)),{},diagnostics);return decoded?copy(*decoded,false):nullptr;
  };
  auto registration=document->registerLifecycleAdapter("NativeDraft",lifetime,callbacks);QVERIFY(registration);auto stamp=window->evaluationBinding->stamp();std::string error;
  auto one=document->createConsumerTransient("NativeDraft","one",definition("stable"),stamp,&error);QVERIFY2(one,qPrintable(QString::fromStdString(error)));
  auto two=document->createConsumerTransient("NativeDraft","two",definition("stable"),stamp,&error);QVERIFY(two);QVERIFY(!document->createConsumerTransient("NativeDraft","one",definition("stable"),stamp));
  QVERIFY(!document->createConsumerTransient("Section","invalid",definition("invalid"),stamp));QVERIFY(!document->drafts().at(*one).before->source);
  Design::FieldChange change{Design::ChangeKind::LiteralField};change.fieldPath="invalid";change.literal=std::make_shared<const Value>(Value(3.));
  auto previous=document->drafts().at(*one).preview;QVERIFY(!document->updateDraft(*one,{change}));QVERIFY(document->drafts().at(*one).preview==previous);
  change.fieldPath="adjust.offset";QVERIFY(document->updateDraft(*one,{change}));QCOMPARE(std::dynamic_pointer_cast<const Design::Section>(document->drafts().at(*one).preview->evaluated)->offset,3.);
  QVERIFY(!document->drafts().at(*one).before->definition.toObject().contains("adjust"));
  QCOMPARE(editor->toPlainText(),QString("cube(1);\n"));QVERIFY(document->promoteConsumer(*one));auto promoted=editor->toPlainText();QVERIFY(promoted.contains("offset : 3"));QVERIFY(document->drafts().empty());
  editor->undo();QCOMPARE(editor->toPlainText(),QString("cube(1);\n"));QVERIFY(document->drafts().count(*one));QCOMPARE(std::dynamic_pointer_cast<const Design::Section>(document->drafts().at(*one).preview->evaluated)->offset,3.);
  editor->redo();QCOMPARE(editor->toPlainText(),promoted);QVERIFY(document->drafts().empty());QVERIFY(compile(*window));
  stamp=window->evaluationBinding->stamp();QVERIFY(!document->createConsumerTransient("NativeDraft","one",definition("stable"),stamp));
  auto fresh=document->createConsumerTransient("NativeDraft","new",definition("stable"),stamp);QVERIFY(fresh);delete lifetime;QVERIFY(document->drafts().empty());QVERIFY(!document->promoteConsumer(*fresh));
  editor->setContentModified(false);window->tabManager->closeCurrentTab();
}
void TestDesignObjects::personalViewsPromotionAndCamera(){
  window->tabManager->actionNew();auto *editor=window->activeEditor;editor->setPlainText("@{type: View} front={id:\"source-view\",direction:[0,1,0],up:[0,0,1]};cube([20,30,40]);\n");QVERIFY(compile(*window));
  auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());Tree tree(scene->root,scene->documentPath);GeometryEvaluator evaluator(tree);auto geometry=evaluator.evaluateGeometry(*scene->root,false);QVERIFY(geometry);
  Design::ResolveContext context{"r","i","c",geometry->getBoundingBox(),scene->designObjects->records,{}};auto fit=Design::viewCamera(*scene->designObjects->records.at("source-view"),context);QVERIFY(fit);QVERIFY(fit->getVpt().isApprox(Vector3d(10,15,20)));
  window->inspectorDock->show();window->inspectorDock->refresh();auto *apply=window->inspectorDock->findChild<QPushButton *>("applyDesignView");QVERIFY(apply);QTest::mouseClick(apply,Qt::LeftButton);QTRY_VERIFY(window->qglview->cam.getVpt().isApprox(Vector3d(10,15,20)));
  auto store=SavedViews::forEditor(editor);auto before=store->serialize();QVERIFY(!before.contains("source-view"));
  auto pose=window->qglview->navigation()->current();auto id=store->add(pose);QVERIFY(!id.isEmpty());auto entry=store->find(id);QVERIFY(entry);QVERIFY(entry->record);QCOMPARE(entry->record->kind,std::string("View"));
  auto *chooser=window->inspectorDock->findChild<QComboBox *>("designViewList");QVERIFY(chooser);int row=-1;for(int i=0;i<chooser->count();++i)if(chooser->itemData(i).toStringList().value(0)==id)row=i;QVERIFY(row>=0);chooser->setCurrentIndex(row);
  auto *promote=window->inspectorDock->findChild<QPushButton *>("promoteDesignView");QVERIFY(promote);QTest::mouseClick(promote,Qt::LeftButton);QTRY_VERIFY(!store->find(id));
  QVERIFY(!store->find(id));QVERIFY(editor->toPlainText().contains(id));editor->undo();QVERIFY(store->find(id));QVERIFY(!editor->toPlainText().contains(id));editor->redo();QVERIFY(!store->find(id));QVERIFY(compile(*window));
  scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());QVERIFY(scene->designObjects->records.count(id.toStdString()));
  Design::ResolveContext fixedContext{"r","i","c",geometry->getBoundingBox(),scene->designObjects->records,{}};
  auto fixed=Design::viewCamera(*scene->designObjects->records.at(id.toStdString()),fixedContext);QVERIFY(fixed);QVERIFY(CameraPose::matrix(fixed->object_rot).isApprox(CameraPose::matrix(pose.rotation),1e-9));QVERIFY(fixed->object_trans.isApprox(pose.translation));QCOMPARE(fixed->viewer_distance,pose.distance);
  QTemporaryDir directory;auto path=directory.filePath("views.scad");QVERIFY(window->tabManager->saveAs(editor,path));QVERIFY(!read(SavedViews::companion(path)).contains(id.toUtf8()));
  auto companion=read(SavedViews::companion(path));window->tabManager->closeCurrentTab();window->tabManager->open(path);QVERIFY(compile(*window));QVERIFY(SavedViews::forEditor(window->activeEditor)->entries().isEmpty());QCOMPARE(read(SavedViews::companion(path)),companion);window->tabManager->closeCurrentTab();
}
void TestDesignObjects::documentSavesConflictsAndPartialOutcomes(){
  QTemporaryDir directory;auto first=directory.filePath("one.scad"),second=directory.filePath("two.scad"),root=directory.filePath("root.scad");
  auto one=sourceSection();one.replace("middle","one");auto two=sourceSection();two.replace("middle","two");
  QVERIFY(write(first,one));QVERIFY(write(second,two));QVERIFY(write(root,"include <one.scad> include <two.scad> cube(1);"));
  window->tabManager->open(first);auto *firstEditor=window->activeEditor;window->tabManager->open(second);auto *secondEditor=window->activeEditor;window->tabManager->open(root);auto *rootEditor=window->activeEditor;QVERIFY(compile(*window));
  Design::DesignEdits edits(window);auto stamp=window->evaluationBinding->stamp();Design::FieldChange label{Design::ChangeKind::Label};label.label="Edited";auto firstEdit=edits.prepare("one",{label},stamp),secondEdit=edits.prepare("two",{label},stamp);QVERIFY(firstEdit && secondEdit);
  QCOMPARE(SourceEditing::apply(*firstEditor,firstEdit.request).error,SourceEditing::EditError::None);QCOMPARE(SourceEditing::apply(*secondEditor,secondEdit.request).error,SourceEditing::EditError::None);
  auto outcomes=Design::DesignDocument::saveSequentially(*window->tabManager,{firstEditor,secondEditor},[&](EditorInterface& editor){return &editor==firstEditor?window->tabManager->save(&editor):false;});
  QCOMPARE(outcomes.size(),size_t(2));QVERIFY(outcomes[0].saved);QVERIFY(!outcomes[1].saved);QVERIFY(read(first).contains("Edited"));QCOMPARE(read(second),two);QVERIFY(secondEditor->isContentModified());
  auto retry=Design::DesignDocument::saveSequentially(*window->tabManager,{secondEditor});QVERIFY(retry.size()==1 && retry[0].saved);QVERIFY(read(second).contains("Edited"));
  secondEditor->insertOrReplaceText("// local\n");auto local=secondEditor->toPlainText();QVERIFY(write(second,"// external\n"+two));
  {CancelDialogs dialogs;QVERIFY(!window->tabManager->save(secondEditor));QVERIFY(dialogs.seen);}QCOMPARE(secondEditor->toPlainText(),local);QVERIFY(read(second).startsWith("// external"));
  auto copyPath=directory.filePath("copy.scad");QVERIFY(window->tabManager->saveACopy(secondEditor,copyPath));QCOMPARE(secondEditor->filepath,second);QVERIFY(secondEditor->isContentModified());auto copied=read(copyPath);copied.replace("\r\n","\n");QCOMPARE(copied,local.toUtf8());
  auto asPath=directory.filePath("retained.scad");QVERIFY(window->tabManager->saveAs(secondEditor,asPath));QCOMPARE(secondEditor->filepath,asPath);
  auto views=SavedViews::forEditor(secondEditor);auto personal=views->add(window->qglview->navigation()->current());QVERIFY(!personal.isEmpty());QVERIFY(window->tabManager->save(secondEditor));views->rename(personal,"Local name");
  QVERIFY(write(SavedViews::companion(asPath),"external companion"));auto buffer=secondEditor->toPlainText();
  {CancelDialogs dialogs;QVERIFY(!window->tabManager->save(secondEditor));}QCOMPARE(secondEditor->toPlainText(),buffer);QCOMPARE(read(SavedViews::companion(asPath)),QByteArray("external companion"));QVERIFY(views->dirty());
  auto recover=directory.filePath("recovered.scad");QVERIFY(window->tabManager->saveAs(secondEditor,recover));
  window->tabManager->open(recover);window->tabManager->closeCurrentTab();window->tabManager->open(first);window->tabManager->closeCurrentTab();window->tabManager->open(root);QVERIFY(window->activeEditor==rootEditor);window->tabManager->closeCurrentTab();
}
