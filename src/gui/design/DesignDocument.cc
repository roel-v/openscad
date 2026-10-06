#include "gui/design/DesignDocument.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignReferences.h"
#include "core/SourceFile.h"
#include "gui/Editor.h"
#include "gui/ScintillaEditor.h"
#include "gui/MainWindow.h"
#include "gui/TabManager.h"
#include "gui/SavedViews.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/parameter/ParameterWidget.h"
#include <QUuid>
#include <QTimer>
#include <algorithm>

namespace Design {
namespace {
std::string uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
Value vector(const Vector3d& value) { VectorType result(nullptr); for (int i=0;i<3;++i) result.emplace_back(Value(value[i])); return Value(std::move(result)); }
std::shared_ptr<const Record> isolated(const Record& before){
  auto result=std::make_shared<Record>();result->id=before.id;result->kind=before.kind;result->label=before.label;result->source=before.source;
  result->definition=ownedValue(before.definition);result->evaluated=before.evaluated;
  for(const auto& dependency:before.dependencies)result->dependencies.push_back({dependency.domain,dependency.id,ownedValue(dependency.data)});
  return result;
}
}
DesignDocument::Token::Token(Token&& other) noexcept : document(other.document), kind(std::move(other.kind)) { other.document.clear(); }
DesignDocument::Token& DesignDocument::Token::operator=(Token&& other) noexcept { if (this!=&other) { reset(); document=other.document; kind=std::move(other.kind); other.document.clear(); } return *this; }
void DesignDocument::Token::reset() { if (document) document->unregisterAdapter(kind); document.clear(); }
DesignDocument::DesignDocument(EditorInterface *owner, MainWindow *window) : QObject(owner), owner(owner), window(window), edits(window) {
  setObjectName("designDocument");
  connect(window->evaluationBinding, &Selection::EvaluationBinding::invalidated, this, [this] { clearTemporaryDrafts(); emit changed(); });
  connect(window->evaluationBinding, &Selection::EvaluationBinding::installed, this, &DesignDocument::synchronize);
  connect(owner, &EditorInterface::contentsChanged, this, [this] { clearTemporaryDrafts(); emit changed(); });
  connect(owner, &EditorInterface::externalEditStarting, this, [this] {
    sourceMutation=true;
    QTimer::singleShot(0,this,[this]{sourceMutation=false;emit changed();});
  });
  connect(owner->parameterWidget,&ParameterWidget::parametersChanged,this,[this]{++environmentRevision;emit changed();});
  connect(&sourceInputWatcher,&QFileSystemWatcher::fileChanged,this,[this]{++environmentRevision;emit changed();});
  synchronize();
}
void DesignDocument::clearTemporaryDrafts(){
  for(auto it=editing.begin();it!=editing.end();)if(!restoredConsumers.count(it->first))it=editing.erase(it);else ++it;
}
DesignDocument *DesignDocument::forEditor(EditorInterface& editor, MainWindow& window) {
  if (auto *document=editor.findChild<DesignDocument *>(QString(),Qt::FindDirectChildrenOnly)) return document;
  return new DesignDocument(&editor,&window);
}
bool DesignDocument::valid(const Selection::Stamp& stamp) const {
  return owner && window && stamp.documentId==owner->documentId() && window->evaluationBinding->validate(stamp);
}
bool DesignDocument::independentSection(const std::shared_ptr<const Record>& record) const {
  if(!owner || !window || window->activeEditor!=owner || !record || record->source ||
     record->kind!="Section" || !record->dependencies.empty())return false;
  const auto found=transient.find(record->id);
  return found!=transient.end() && found->second==record;
}
bool DesignDocument::validDraft(const Draft& draft) const {
  return (!draft.consumer && (independentSection(draft.before) ||
    (sourceStateCurrent() && draft.before->source))) || valid(draft.stamp);
}
DesignDocument::Token DesignDocument::registerLifecycleAdapter(std::string name, QObject *lifetime, LifecycleCallbacks callbacks) {
  auto handler=kind(name);
  if (!lifetime || !handler || handler->destination!=RecordDestination::Consumer || adapters.count(name) || !callbacks.resolveConsumerRecord || !callbacks.prepareInsert || !callbacks.prepareEdit || !callbacks.locatePromotedRecord) return {};
  auto connection=connect(lifetime,&QObject::destroyed,this,[this,name] { unregisterAdapter(name); });
  adapters.emplace(name,Adapter{lifetime,std::move(callbacks),connection}); return Token(this,std::move(name));
}
void DesignDocument::unregisterAdapter(const std::string& name) {
  auto found=adapters.find(name); if (found==adapters.end()) return;
  disconnect(found->second.lifetime); adapters.erase(found);
  for (auto it=editing.begin();it!=editing.end();) if (it->second.consumer && it->second.kind==name) {restoredConsumers.erase(it->first);it=editing.erase(it);} else ++it;
  for (auto it=pendingConsumers.begin();it!=pendingConsumers.end();) if (it->second.kind==name) it=pendingConsumers.erase(it); else ++it;
  emit changed();
}
std::shared_ptr<const Record> DesignDocument::consumerRecord(const std::string& name, const std::string& key, const Selection::Stamp& stamp) const {
  if (!valid(stamp)) return {};
  auto found=adapters.find(name); if (found==adapters.end() || !found->second.owner) return {};
  auto callback=found->second.callbacks.resolveConsumerRecord;QPointer<QObject> lifetime=found->second.owner;QPointer<DesignDocument> self(const_cast<DesignDocument *>(this));
  auto record=callback(key,stamp);
  return self && lifetime && valid(stamp) && record && record->kind==name ? record : nullptr;
}
std::optional<std::string> DesignDocument::createTransient(const std::string& name, Value definition, std::string *error) {
  auto fail=[&](const std::string& message)->std::optional<std::string> { if (error) *error=message; return {}; };
  auto handler=kind(name);
  if (!handler || handler->destination!=RecordDestination::DocumentRegistry || (name!="Section" && name!="View")) return fail("Kind requires its own lifecycle adapter");
  if (definition.type()!=Value::Type::OBJECT) return fail("Transient definition must be an object");
  auto value=ownedValue(definition); auto object=value.toObject().clone();
  if (!object.contains("id")) object.set("id",Value(uuid()));
  value=Value(std::move(object)); std::vector<Diagnostic> diagnostics;
  auto record=makeRecord(*handler,value,{{"","",name},DeclarationScope::File},diagnostics);
  if (!record) return fail(diagnostics.empty()?"Invalid definition":diagnostics.front().message);
  if (transient.count(record->id)) return fail("Transient id already exists");
  if (window && window->evaluationBinding->current()) { auto all=records(window->evaluationBinding->stamp()); if (all.count(record->id)) return fail("Design id already exists"); }
  auto mutableRecord=std::make_shared<Record>(); mutableRecord->id=record->id;mutableRecord->kind=record->kind;mutableRecord->label=record->label;
  mutableRecord->definition=ownedValue(record->definition);mutableRecord->evaluated=record->evaluated;
  for (const auto& token:record->dependencies) mutableRecord->dependencies.push_back({token.domain,token.id,ownedValue(token.data)});
  transient.emplace(record->id,std::move(mutableRecord));emit changed();return record->id;
}
Records DesignDocument::records(const Selection::Stamp& stamp) const {
  auto scene=valid(stamp)?window->evaluationBinding->scene(stamp):nullptr;
  Records result=sourceStateCurrent()?sourceState.records:scene && scene->designObjects?scene->designObjects->records:Records{};
  for (const auto& [id,record]:transient) { if (result.count(id)) result.erase(id); else result.emplace(id,record); }
  return result;
}
bool DesignDocument::sourceStateCurrent() const {
  if(!owner || !window || window->activeEditor!=owner || sourceState.documentId!=owner->documentId() || sourceState.inputs.empty() || sourceState.environmentRevision!=environmentRevision)return false;
  for(const auto& [key,input]:sourceState.inputs){
    bool matched=false;
    for(auto editor:window->tabManager->editorList)if(input.editorId && editor->documentId().toStdString()==*input.editorId){
      if(editor->sourceFingerprint.toStdString()!=input.fingerprint)return false;
      if(!editor->filepath.isEmpty() && EvaluationInputs::fileKey(editor->filepath.toStdString())!=key)return false;
      matched=true;break;
    }
    if(input.editorId && !matched)return false;
  }
  return true;
}
Records DesignDocument::sourceDisplayRecords() const {
  return owner && owner->documentId()==sourceState.documentId?sourceState.records:Records{};
}
PreparedEdit DesignDocument::prepareSourceEdit(const Record& record,const std::vector<FieldChange>& changes) const {
  if(!sourceStateCurrent() || !record.source)return {{},{},"Source changed outside the cross section controls. Preview to refresh it."};
  for(const auto& [key,input]:sourceState.inputs)if(!input.editorId)try{
    if(EvaluationInputs::fingerprint(EvaluationInputs::diskBytes(key))!=input.fingerprint)return {{},{},"An included file changed. Preview to refresh it."};
  }catch(...){return {{},{},"An included file is unavailable. Preview after restoring it."};}
  auto input=sourceState.inputs.find(record.source->fileKey);
  if(input==sourceState.inputs.end())return {{},{},"Source document is not captured"};
  return edits.prepareCurrent(record,changes,input->second);
}
std::optional<std::string> DesignDocument::beginEdit(const std::string& id, const Selection::Stamp& stamp) {
  auto all=records(stamp);
  if(!valid(stamp) && !sourceStateCurrent()){
    const auto local=transient.find(id);
    if(local==transient.end() || !independentSection(local->second))return {};
    all.emplace(*local);
  }
  auto found=all.find(id);if(found==all.end()) return {};
  for (const auto& [token,draft]:editing) if (!draft.consumer && draft.key==id) return {};
  auto token=uuid();editing.emplace(token,Draft{token,id,found->second->kind,found->second,found->second,{},stamp,false});return token;
}
std::optional<std::string> DesignDocument::beginConsumerEdit(const std::string& name,const std::string& key,const Selection::Stamp& stamp) {
  auto record=consumerRecord(name,key,stamp);if(!record)return {};
  for (const auto& [token,draft]:editing) if(draft.consumer && draft.kind==name && draft.key==key)return {};
  auto token=uuid();editing.emplace(token,Draft{token,key,name,record,record,{},stamp,true});return token;
}
std::optional<std::string> DesignDocument::createConsumerTransient(const std::string& name,const std::string& key,Value definition,const Selection::Stamp& stamp,std::string *error) {
  auto fail=[&](const std::string& message)->std::optional<std::string>{if(error)*error=message;return {};};
  auto handler=kind(name);auto adapter=adapters.find(name);
  if(!valid(stamp) || !handler || handler->destination!=RecordDestination::Consumer || adapter==adapters.end() || !adapter->second.owner)
    return fail("A current evaluation and registered consumer lifecycle owner are required");
  if(key.empty())return fail("A consumer draft requires an opaque owner key");
  for(const auto& [token,draft]:editing)if(draft.consumer && draft.kind==name && draft.key==key)return fail("Consumer draft already exists");
  for(const auto& [token,draft]:pendingConsumers)if(draft.kind==name && draft.key==key)return fail("Consumer promotion is pending");
  QPointer<DesignDocument> self(this);auto existing=consumerRecord(name,key,stamp);
  if(!self)return fail("Document closed during lifecycle callback");
  if(!valid(stamp) || !adapters.count(name) || !adapters.at(name).owner)return fail("Lifecycle callback changed its owner or evaluation");
  if(existing)return fail("Consumer declaration already exists");
  std::vector<Diagnostic> diagnostics;auto decoded=makeRecord(*handler,definition,{{"","",key},DeclarationScope::File},diagnostics);
  if(!decoded)return fail(diagnostics.empty()?"Invalid consumer definition":diagnostics.front().message);
  auto record=std::make_shared<Record>();record->id=decoded->id;record->kind=decoded->kind;record->label=decoded->label;
  record->definition=ownedValue(decoded->definition);record->evaluated=decoded->evaluated;
  for(const auto& dependency:decoded->dependencies)record->dependencies.push_back({dependency.domain,dependency.id,ownedValue(dependency.data)});
  auto token=uuid();editing.emplace(token,Draft{token,key,name,record,record,{},stamp,true});emit changed();return token;
}
std::shared_ptr<const Record> DesignDocument::updated(const Record& record,const std::vector<FieldChange>& changes,std::string& error) const {
  auto value=ownedValue(record.definition);auto object=value.toObject().clone();
  for (const auto& change:changes) {
    if (change.kind==ChangeKind::Label) object.set("label",Value(change.label));
    else if (record.kind=="Section" && (change.kind==ChangeKind::Translation || change.kind==ChangeKind::Rotation || change.kind==ChangeKind::Offset)) {
      ObjectType adjust(nullptr);if(object.get("adjust").type()==Value::Type::OBJECT)adjust=object.get("adjust").toObject().clone();
      if(change.kind==ChangeKind::Translation)adjust.set("translation",vector(change.translation));
      else if(change.kind==ChangeKind::Offset)adjust.set("offset",Value(change.offset));
      else { VectorType quaternion(nullptr);for(double component:{change.rotation.x(),change.rotation.y(),change.rotation.z(),change.rotation.w()})quaternion.emplace_back(Value(component));adjust.set("rotation",Value(std::move(quaternion))); }
      object.set("adjust",Value(std::move(adjust)));
    } else { error="Field requires a kind-specific edit adapter";return {}; }
  }
  std::vector<Diagnostic> diagnostics; auto handler=kind(record.kind);
  auto result=makeRecord(*handler,Value(std::move(object)),{record.source.value_or(DeclarationKey{"","",record.label}),DeclarationScope::File},diagnostics);
  if (!result)error=diagnostics.empty()?"Invalid draft":diagnostics.front().message;
  else if(!record.source){
    // A synthetic decode location must not turn a session-only preview into source-backed data.
    auto preview=std::make_shared<Record>();preview->id=result->id;preview->kind=result->kind;preview->label=result->label;
    preview->definition=ownedValue(result->definition);preview->evaluated=result->evaluated;
    for(const auto& dependency:result->dependencies)preview->dependencies.push_back({dependency.domain,dependency.id,ownedValue(dependency.data)});
    return preview;
  }
  return result;
}
LifecycleResult DesignDocument::updateDraft(const std::string& token,const std::vector<FieldChange>& changes) {
  auto found=editing.find(token);if(found==editing.end())return {false,"Draft no longer exists"};
  if(!validDraft(found->second)){editing.erase(found);return {false,"Draft evaluation became stale"};}
  std::string error;auto merged=found->second.changes;
  for(const auto& change:changes){
    // Built-in fields are absolute values. Retain only their latest gesture value;
    // consumer adapters continue to receive their ordered change stream.
    auto previous=std::find_if(merged.begin(),merged.end(),[&](const auto& item){return !found->second.consumer && item.kind==change.kind;});
    if(previous==merged.end())merged.push_back(change);else *previous=change;
  }
  if(found->second.consumer) {
    auto draft=found->second;auto adapter=adapters.find(draft.kind);
    if(adapter==adapters.end() || !adapter->second.owner)return {false,"Lifecycle owner closed"};
    auto callback=adapter->second.callbacks.previewConsumerRecord;
    if(callback) {
      QPointer<DesignDocument> self(this);QPointer<QObject> lifetime=adapter->second.owner;
      // ObjectType::clone retains shared mutable storage. Isolate both callback input and published output.
      auto input=isolated(*draft.before);auto preview=callback(*input,merged,error);
      if(!self)return {false,"Document closed during lifecycle callback"};
      found=editing.find(token);
      if(!lifetime || found==editing.end() || !valid(draft.stamp))return {false,"Lifecycle callback changed its owner or evaluation"};
      if(!preview || preview->kind!=draft.kind || preview->id!=draft.before->id)return {false,error.empty()?"Consumer preview changed its kind or identity":error};
      found->second.preview=isolated(*preview);
    } else if(std::any_of(merged.begin(),merged.end(),[](const auto& change){return change.kind==ChangeKind::LiteralField;}))
      return {false,"Literal consumer fields require a registered preview validator"};
    found->second.changes=std::move(merged);emit changed();return {true,{}};
  }
  auto preview=updated(*found->second.before,merged,error);if(!preview)return {false,error};
  found->second.preview=std::move(preview);found->second.changes=std::move(merged);emit changed();return {true,{}};
}
void DesignDocument::cancelDraft(const std::string& token){restoredConsumers.erase(token);if(editing.erase(token))emit changed();}
LifecycleResult DesignDocument::changeTransient(const std::string& id,std::shared_ptr<const Record> replacement){
  if(!owner)return {false,"Document closed"};auto found=transient.find(id);if(found==transient.end())return {false,"Transient id is missing"};
  auto before=found->second;QPointer<DesignDocument> guard(this);auto participant=std::make_shared<SourceEditing::UndoParticipant>();participant->lifetime=this;
  // Session transactions emit externalEditStarting too. Their participants finish
  // synchronously; do not leave consumers suppressed until the timer fallback.
  participant->undo=[guard,id,before]{if(guard){guard->transient.insert_or_assign(id,before);guard->sourceMutation=false;emit guard->changed();}};
  participant->redo=[guard,id,replacement]{if(guard){if(replacement)guard->transient.insert_or_assign(id,replacement);else guard->transient.erase(id);guard->sourceMutation=false;emit guard->changed();}};
  SourceEditing::EditRequest request{owner->documentId(),owner->sourceRevision,owner->sourceFingerprint,{},participant};
  auto result=SourceEditing::apply(*owner,request);return {result.changed,result.error==SourceEditing::EditError::None?std::string():result.detail.toStdString()};
}
LifecycleResult DesignDocument::commitDraft(const std::string& token){
  QPointer<DesignDocument> self(this);
  auto found=editing.find(token);if(found==editing.end())return {false,"Draft no longer exists"};
  auto draft=found->second;if(!validDraft(draft))return {false,"Draft evaluation became stale"};
  if(!draft.consumer && !draft.before->source){
    auto value=std::make_shared<Record>();value->id=draft.preview->id;value->kind=draft.preview->kind;value->label=draft.preview->label;
    value->definition=ownedValue(draft.preview->definition);value->evaluated=draft.preview->evaluated;
    for(const auto& dependency:draft.preview->dependencies)value->dependencies.push_back({dependency.domain,dependency.id,ownedValue(dependency.data)});
    auto result=changeTransient(draft.key,value);if(self && result)editing.erase(token);return result;
  }
  PreparedEdit prepared;
  if(draft.consumer){auto adapter=adapters.find(draft.kind);if(adapter==adapters.end() || !adapter->second.owner)return {false,"Lifecycle owner closed"};auto callback=adapter->second.callbacks.prepareEdit;prepared=callback(draft.key,draft.changes,draft.stamp);}
  else prepared=prepareSourceEdit(*draft.before,draft.changes);
  if(!prepared)return {false,prepared.error};
  if(!self)return {false,"Document closed during lifecycle callback"};
  if(!validDraft(draft))return {false,"Lifecycle callback changed the evaluation"};
  if(draft.consumer){auto adapter=adapters.find(draft.kind);if(adapter==adapters.end() || !adapter->second.owner)return {false,"Lifecycle owner closed"};}
  if(!draft.consumer){
    auto result=applySourceEdit(std::move(prepared),{{draft.key,draft.preview}},{});
    if(self && result)editing.erase(token);
    return result;
  }
  auto result=SourceEditing::apply(*prepared.editor,prepared.request);if(result.error!=SourceEditing::EditError::None)return {false,result.detail.toStdString()};
  if(self){editing.erase(token);emit changed();}return {result.changed,{}};
}
LifecycleResult DesignDocument::promote(const std::vector<std::string>& ids,EditorInterface& destination,const Selection::Stamp& stamp){
  if(!sourceStateCurrent())return {false,"Preview to refresh source changed outside the cross section controls."};
  auto all=records(stamp);std::set<std::string> included;
  bool destinationCaptured=false;
  for(const auto& [file,input]:sourceState.inputs){included.insert(file);if(input.editorId==destination.documentId().toStdString())destinationCaptured=true;}
  if(!destinationCaptured)return {false,"Destination is outside the captured source documents"};
  auto closure=persistenceClosure(ids,all,included);if(!closure.valid())return {false,closure.diagnostics.front().message};
  auto prepared=edits.insertCurrent(closure.insert,destination,sourceState.names);if(!prepared)return {false,prepared.error};
  Records before;for(const auto& record:closure.insert){auto found=transient.find(record->id);if(found!=transient.end())before.emplace(*found);}
  return applySourceEdit(std::move(prepared),{}, {},before);
}
LifecycleResult DesignDocument::applySourceEdit(PreparedEdit prepared,const Records& replacements,
                                               const std::set<std::string>& removed,const Records& promoted){
  if(!prepared)return {false,prepared.error};
  if(!sourceStateCurrent())return {false,"Source changed outside the cross section controls. Preview to refresh it."};
  const auto before=sourceState;auto after=before;
  const auto editorId=prepared.editor->documentId().toStdString();
  auto input=std::find_if(after.inputs.begin(),after.inputs.end(),[&](const auto& item){return item.second.editorId==editorId;});
  if(input==after.inputs.end())return {false,"Destination is not captured"};
  QVector<SourceEditing::ByteEdit> patches;
  auto check=SourceEditing::validate(prepared.editor->toPlainText().toUtf8(),prepared.editor->documentId(),
    prepared.editor->sourceRevision,prepared.editor->sourceFingerprint,prepared.editor->isSourceReadOnly(),prepared.request,patches);
  if(check.error!=SourceEditing::EditError::None)return {false,check.detail.toStdString()};
  for(const auto& patch:patches)input->second.utf8.replace(patch.begin,patch.end-patch.begin,patch.replacement.toStdString());
  input->second.fingerprint=EvaluationInputs::fingerprint(input->second.utf8);
  for(const auto& id:removed)after.records.erase(id);
  for(const auto& [id,record]:replacements)after.records.insert_or_assign(id,record);
  for(auto& [id,record]:after.records)if(record->source && record->source->fileKey==input->first){
    auto copy=std::const_pointer_cast<Record>(isolated(*record));
    auto& source=*copy->source;
    const auto oldEnd=source.endByte;
    source.beginByte=SourceEditing::transformPosition(source.beginByte,patches);
    source.endByte=SourceEditing::transformPosition(source.endByte,patches);
    for(const auto& patch:patches)if(patch.begin==patch.end && size_t(patch.begin)==oldEnd)source.endByte-=patch.replacement.size();
    source.fingerprint=input->second.fingerprint;record=std::move(copy);
  }
  for(const auto& [id,record]:promoted){
    after.names.insert(declarationName(*record));
    auto declaration=serialize(*record);if(input->second.utf8.find("\r\n")!=std::string::npos){
      std::string crlf;for(char c:declaration){if(c=='\n')crlf+='\r';crlf+=c;}declaration=std::move(crlf);
    }
    // Insertions are exact transaction payloads, not a search through unrelated source.
    bool located=false;
    for(const auto& patch:patches){auto offset=patch.replacement.toStdString().find(declaration);if(offset==std::string::npos)continue;
      auto copy=std::const_pointer_cast<Record>(isolated(*record));
      const auto begin=SourceEditing::transformPosition(patch.begin,patches)-patch.replacement.size()+offset;
      copy->source=DeclarationKey{input->first,input->second.fingerprint,declarationName(*record),DeclarationHost::Assignment,
        size_t(begin),size_t(begin+declaration.size()- (declaration.ends_with("\r\n")?2:1))};
      after.records.insert_or_assign(id,std::move(copy));located=true;break;
    }
    if(!located)return {false,"Inserted declaration could not be located"};
  }
  QPointer<DesignDocument> guard(this);auto participant=std::make_shared<SourceEditing::UndoParticipant>();participant->lifetime=this;
  participant->undo=[guard,before,promoted]{if(guard){guard->sourceState=before;for(const auto& item:promoted)guard->transient.insert_or_assign(item.first,item.second);guard->sourceMutation=false;emit guard->changed();}};
  participant->redo=[guard,after,promoted]{if(guard){guard->sourceState=after;for(const auto& item:promoted)guard->transient.erase(item.first);guard->sourceMutation=false;emit guard->changed();}};
  prepared.request.participant=participant;
  sourceMutation=true;
  auto result=SourceEditing::apply(*prepared.editor,prepared.request);
  if(guard && result.changed && prepared.insertedRange){
    if(auto editor=dynamic_cast<ScintillaEditor *>(prepared.editor.data())){
      // Promotion currently targets the active root. Do not switch documents for
      // other lifecycle users that explicitly choose another destination.
      if(window->activeEditor==editor)window->editorDock->show();
      editor->qsci->SendScintilla(QsciScintillaBase::SCI_SETSEL,
        static_cast<unsigned long>(prepared.insertedRange->first),static_cast<long>(prepared.insertedRange->second));
      editor->qsci->ensureCursorVisible();
    }
  }
  if(guard){sourceMutation=false;emit changed();}
  return {result.changed,result.error==SourceEditing::EditError::None?std::string():result.detail.toStdString()};
}
LifecycleResult DesignDocument::promoteConsumer(const std::string& token){
  auto found=editing.find(token);if(found==editing.end() || !found->second.consumer)return {false,"Consumer draft no longer exists"};
  auto draft=found->second;if(!valid(draft.stamp))return {false,"Draft evaluation became stale"};
  auto adapter=adapters.find(draft.kind);if(adapter==adapters.end() || !adapter->second.owner)return {false,"Lifecycle owner closed"};
  auto callback=adapter->second.callbacks.prepareInsert;QPointer<QObject> lifetime=adapter->second.owner;QPointer<DesignDocument> self(this);
  auto prepared=callback(draft,draft.stamp);if(!prepared)return {false,prepared.error};
  if(!self)return {false,"Document closed during lifecycle callback"};
  if(!lifetime || !valid(draft.stamp) || !adapters.count(draft.kind))return {false,"Lifecycle callback changed its owner or evaluation"};
  std::vector<std::string> required;for(const auto& dependency:draft.preview->dependencies)if(dependency.domain=="design")required.push_back(dependency.id);
  auto scene=window->evaluationBinding->scene(draft.stamp);std::set<std::string> included;for(const auto& [file,input]:scene->inputs->consumed())included.insert(file);
  auto closure=persistenceClosure(required,records(draft.stamp),included);if(!closure.valid())return {false,closure.diagnostics.front().message};
  Records dependencies;
  if(!closure.insert.empty()){
    auto declarations=edits.insert(closure.insert,*prepared.editor,draft.stamp);if(!declarations)return {false,declarations.error};
    for(const auto& declaration:declarations.request.edits){
      bool merged=false;for(auto& edit:prepared.request.edits)if(edit.begin==declaration.begin && edit.end==declaration.end && edit.expected.isEmpty() && declaration.expected.isEmpty()){edit.replacement=declaration.replacement+edit.replacement;merged=true;break;}
      if(!merged)prepared.request.edits.append(declaration);
    }
    for(const auto& record:closure.insert){auto found=transient.find(record->id);if(found!=transient.end())dependencies.emplace(*found);}
  }
  if(prepared.request.participant)return {false,"Consumer request already owns a lifecycle participant"};
  QPointer<DesignDocument> guard(this);auto participant=std::make_shared<SourceEditing::UndoParticipant>();
  participant->lifetime=lifetime;
  participant->undo=[guard,draft,dependencies]{if(guard && guard->adapters.count(draft.kind)){for(const auto& [id,record]:dependencies)guard->transient.insert_or_assign(id,record);guard->pendingConsumers.erase(draft.token);guard->editing.insert_or_assign(draft.token,draft);guard->restoredConsumers.insert(draft.token);emit guard->changed();}};
  participant->redo=[guard,draft,dependencies]{if(guard && guard->adapters.count(draft.kind)){for(const auto& [id,record]:dependencies)guard->transient.erase(id);guard->restoredConsumers.erase(draft.token);guard->editing.erase(draft.token);guard->pendingConsumers.insert_or_assign(draft.token,draft);emit guard->changed();}};
  prepared.request.participant=participant;auto result=SourceEditing::apply(*prepared.editor,prepared.request);
  return {result.changed,result.error==SourceEditing::EditError::None?std::string():result.detail.toStdString()};
}
LifecycleResult DesignDocument::promotePersonal(const std::string& id,EditorInterface& destination,const Selection::Stamp& stamp){
  if(!valid(stamp))return {false,"Evaluation is stale"};auto *store=SavedViews::forEditor(owner);auto *entry=store->find(QString::fromStdString(id));if(!entry)return {false,"Personal view is missing"};
  auto before=*entry;auto all=records(stamp);if(all.count(id))return {false,"Design id already exists"};all.emplace(id,before.record);
  auto scene=window->evaluationBinding->scene(stamp);std::set<std::string> included;for(const auto& [file,input]:scene->inputs->consumed())included.insert(file);
  auto closure=persistenceClosure({id},all,included);if(!closure.valid())return {false,closure.diagnostics.front().message};auto prepared=edits.insert(closure.insert,destination,stamp);if(!prepared)return {false,prepared.error};
  Records dependencies;for(const auto& record:closure.insert){auto found=transient.find(record->id);if(found!=transient.end())dependencies.emplace(*found);}
  QPointer<DesignDocument> guard(this);QPointer<SavedViews> library(store);auto participant=std::make_shared<SourceEditing::UndoParticipant>();participant->lifetime=store;
  participant->undo=[guard,library,before,dependencies]{if(library)library->restore(before);if(guard){for(const auto& [id,record]:dependencies)guard->transient.insert_or_assign(id,record);emit guard->changed();}};
  participant->redo=[guard,library,before,dependencies]{if(library)library->remove(before.id());if(guard){for(const auto& [id,record]:dependencies)guard->transient.erase(id);emit guard->changed();}};
  prepared.request.participant=participant;auto result=SourceEditing::apply(*prepared.editor,prepared.request);return {result.changed,result.error==SourceEditing::EditError::None?std::string():result.detail.toStdString()};
}
LifecycleResult DesignDocument::remove(const std::string& id,const Selection::Stamp& stamp){
  const auto local=transient.find(id);
  if(local!=transient.end() && independentSection(local->second))return changeTransient(id,{});
  if(!valid(stamp) && !sourceStateCurrent())return {false,"Preview to refresh source changed outside the cross section controls."};
  if(transient.count(id))return changeTransient(id,{});
  auto all=records(stamp);auto found=all.find(id);if(found==all.end())return {false,"Design id is missing"};
  auto prepared=prepareSourceEdit(*found->second,{{ChangeKind::DeleteDeclaration}});if(!prepared)return {false,prepared.error};
  return applySourceEdit(std::move(prepared),{},{id});
}
void DesignDocument::synchronize(){
  if(!window || !owner)return;auto stamp=window->evaluationBinding->stamp();if(!valid(stamp))return;
  auto scene=window->evaluationBinding->scene(stamp);
  if(scene && scene->designObjects)sourceState={scene->designObjects->records,scene->inputs->consumed(),owner->documentId(),environmentRevision};
  if(scene && scene->backing && scene->backing->parsedRoot)for(const auto& assignment:scene->backing->parsedRoot->scope->assignmentOccurrences)sourceState.names.insert(assignment->getName());
  for(const auto& connection:sourceConnections)disconnect(connection);sourceConnections.clear();
  for(auto editor:window->tabManager->editorList)if(editor!=owner){
    bool captured=false;for(const auto& [key,input]:sourceState.inputs)if(input.editorId==editor->documentId().toStdString())captured=true;
    if(captured)sourceConnections.push_back(connect(editor,&EditorInterface::externalEditStarting,this,[this]{
      sourceMutation=true;QTimer::singleShot(0,this,[this]{sourceMutation=false;emit changed();});
    }));
  }
  if(!sourceInputWatcher.files().empty())sourceInputWatcher.removePaths(sourceInputWatcher.files());
  QStringList paths;for(const auto& [key,input]:sourceState.inputs)if(!input.editorId)paths.append(QString::fromStdString(key));
  if(!paths.empty() && !sourceInputWatcher.addPaths(paths).empty())++environmentRevision;
  clearTemporaryDrafts();for(auto& [token,draft]:editing)if(restoredConsumers.count(token))draft.stamp=stamp;
  auto pending=pendingConsumers;QPointer<DesignDocument> self(this);
  for(const auto& [token,draft]:pending){
    auto adapter=adapters.find(draft.kind);
    if(adapter==adapters.end() || !adapter->second.owner){pendingConsumers.erase(token);continue;}
    auto callback=adapter->second.callbacks.locatePromotedRecord;QPointer<QObject> lifetime=adapter->second.owner;
    auto record=callback(draft,stamp);if(!self)return;
    if(lifetime && valid(stamp) && record && record->kind==draft.kind)pendingConsumers.erase(token);
  }
  emit changed();
}
std::vector<SaveOutcome> DesignDocument::saveSequentially(TabManager& tabs,const std::vector<EditorInterface *>& editors,const std::function<bool(EditorInterface&)>& save){
  std::vector<SaveOutcome> outcomes;std::set<EditorInterface *> seen;
  for(auto *editor:editors){
    if(!editor || !seen.insert(editor).second)continue;
    if(!tabs.editorList.contains(editor)){outcomes.push_back({editor,false,"Document is not open"});break;}
    QPointer<EditorInterface> guard(editor);bool success=save?save(*editor):tabs.save(editor);
    outcomes.push_back({guard,success,success?QString():QString("Save canceled or failed; preceding saves remain committed")});if(!success)break;
  }
  return outcomes;
}
}
