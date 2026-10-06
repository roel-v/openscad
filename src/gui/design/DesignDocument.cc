#include "gui/design/DesignDocument.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignReferences.h"
#include "gui/Editor.h"
#include "gui/MainWindow.h"
#include "gui/TabManager.h"
#include "gui/SavedViews.h"
#include "gui/selection/EvaluationBinding.h"
#include <QUuid>
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
  return (!draft.consumer && independentSection(draft.before)) || valid(draft.stamp);
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
  if (!valid(stamp)) return {};
  auto scene=window->evaluationBinding->scene(stamp); Records result=scene && scene->designObjects ? scene->designObjects->records : Records{};
  for (const auto& [id,record]:transient) { if (result.count(id)) result.erase(id); else result.emplace(id,record); }
  return result;
}
std::optional<std::string> DesignDocument::beginEdit(const std::string& id, const Selection::Stamp& stamp) {
  auto all=records(stamp);
  if(!valid(stamp)){
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
  participant->undo=[guard,id,before]{if(guard){guard->transient.insert_or_assign(id,before);emit guard->changed();}};
  participant->redo=[guard,id,replacement]{if(guard){if(replacement)guard->transient.insert_or_assign(id,replacement);else guard->transient.erase(id);emit guard->changed();}};
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
  else prepared=edits.prepare(*draft.before,draft.changes,draft.stamp);
  if(!prepared)return {false,prepared.error};
  if(!self)return {false,"Document closed during lifecycle callback"};
  if(!valid(draft.stamp))return {false,"Lifecycle callback changed the evaluation"};
  if(draft.consumer){auto adapter=adapters.find(draft.kind);if(adapter==adapters.end() || !adapter->second.owner)return {false,"Lifecycle owner closed"};}
  auto result=SourceEditing::apply(*prepared.editor,prepared.request);if(result.error!=SourceEditing::EditError::None)return {false,result.detail.toStdString()};
  if(self){editing.erase(token);emit changed();}return {result.changed,{}};
}
LifecycleResult DesignDocument::promote(const std::vector<std::string>& ids,EditorInterface& destination,const Selection::Stamp& stamp){
  if(!valid(stamp))return {false,"Evaluation is stale"};
  auto all=records(stamp);auto scene=window->evaluationBinding->scene(stamp);std::set<std::string> included;
  for(const auto& [file,input]:scene->inputs->consumed())included.insert(file);
  auto closure=persistenceClosure(ids,all,included);if(!closure.valid())return {false,closure.diagnostics.front().message};
  auto prepared=edits.insert(closure.insert,destination,stamp);if(!prepared)return {false,prepared.error};
  Records before;for(const auto& record:closure.insert){auto found=transient.find(record->id);if(found!=transient.end())before.emplace(*found);}
  QPointer<DesignDocument> guard(this);auto participant=std::make_shared<SourceEditing::UndoParticipant>();participant->lifetime=this;
  participant->undo=[guard,before]{if(guard){for(const auto& [id,record]:before)guard->transient.insert_or_assign(id,record);emit guard->changed();}};
  participant->redo=[guard,before]{if(guard){for(const auto& [id,record]:before)guard->transient.erase(id);emit guard->changed();}};
  prepared.request.participant=participant;
  auto result=SourceEditing::apply(*prepared.editor,prepared.request);return {result.changed,result.error==SourceEditing::EditError::None?std::string():result.detail.toStdString()};
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
  if(!valid(stamp))return {false,"Evaluation is stale"};
  if(transient.count(id))return changeTransient(id,{});
  auto all=records(stamp);auto found=all.find(id);if(found==all.end())return {false,"Design id is missing"};
  auto prepared=edits.prepare(*found->second,{{ChangeKind::DeleteDeclaration}},stamp);if(!prepared)return {false,prepared.error};
  auto result=SourceEditing::apply(*prepared.editor,prepared.request);return {result.changed,result.error==SourceEditing::EditError::None?std::string():result.detail.toStdString()};
}
void DesignDocument::synchronize(){
  if(!window || !owner)return;auto stamp=window->evaluationBinding->stamp();if(!valid(stamp))return;
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
