#include "gui/anchors/AnchorController.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/selection/ProvenanceService.h"
#include "gui/semantic/SemanticController.h"
#include "core/SourceFile.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QUuid>
#include <cctype>
namespace Anchors {
std::string opaqueKey(const ReferenceKey& key){return QJsonDocument(QJsonObject{{"role",int(key.owner.role)},{"owner",QString::fromStdString(key.owner.id)},{"id",QString::fromStdString(key.id)}}).toJson(QJsonDocument::Compact).toStdString();}
std::optional<ReferenceKey> referenceKey(const std::string& text){auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(text));if(!doc.isObject())return {};auto object=doc.object();const int role=object.value("role").toInt(-1);if(object.size()!=3 || role<0 || role>2 || !object.value("owner").isString() || !object.value("id").isString() || object.value("id").toString().isEmpty())return {};return ReferenceKey{{OwnerRole(role),object.value("owner").toString().toStdString()},object.value("id").toString().toStdString()};}
namespace {
std::vector<LiteralChange> changes(const std::vector<Design::FieldChange>& input,std::string& error){
  std::vector<LiteralChange> result;for(const auto& value:input){
    if(value.kind==Design::ChangeKind::Label)result.push_back({"label",std::make_shared<const Value>(Value(value.label))});
    else if(value.kind==Design::ChangeKind::LiteralField && value.literal)result.push_back({value.fieldPath,std::make_shared<const Value>(Design::ownedValue(*value.literal))});
    else {error="Unsupported reference edit";return {};}
  }return result;
}
}
Controller::Controller(MainWindow *window):QObject(window),window(window){
  setObjectName("anchorController");
  connect(window->evaluationBinding,&Selection::EvaluationBinding::installed,this,&Controller::refresh);
  connect(window->evaluationBinding,&Selection::EvaluationBinding::invalidated,this,&Controller::refresh);
  connect(window->semanticController->targetService(),&Semantic::TargetService::changed,this,&Controller::refreshWidgets);
  inspector=window->inspectorDock->registerContribution({"anchors",30,this,[this](const auto&,const auto&){return buildInspector();}});
  overlay=window->viewportOverlays->registerOverlay(this,Selection::OverlayLayer::WorldOverlay,30,[this](const auto&){drawOverlay();});
}
Controller::~Controller(){if(service)removeReferences(service->resultId());if(window && window->viewportTools)window->viewportTools->finish(tool);overlay.reset();inspector.reset();documents.clear();}
void Controller::report(const std::string& message){if(window && !message.empty())window->statusBar()->showMessage(QString::fromStdString(message),10000);}
void Controller::refreshWidgets(){if(window){prepareOverlay();window->inspectorDock->refresh();window->qglview->update();}}
void Controller::refresh(){
  if(refreshing || !window)return;QScopedValueRollback<bool> guard(refreshing,true);
  const auto stamp=window->evaluationBinding->stamp();
  if(service && window->evaluationBinding->current() && bound.resultId==stamp.resultId && bound.documentId==stamp.documentId){bound=stamp;refreshWidgets();return;}
  if(service)removeReferences(service->resultId());service.reset();sharedEdits.clear();captureToken.reset();
  if(window->evaluationBinding->current()){
    bound=stamp;auto scene=window->evaluationBinding->scene(stamp);
    if(scene && scene->semantic && scene->semantic->valid()){
      QPointer<Semantic::TargetService> targets(window->semanticController->targetService());
      QPointer<Selection::EvaluationBinding> binding(window->evaluationBinding);
      service=std::make_shared<References>(stamp.resultId.toStdString(),scene->semantic,stamp.sourceFingerprint.toStdString(),stamp.configurationFingerprint.toStdString(),[targets](Semantic::TargetKey key){if(targets)return targets->resolve(std::move(key));Semantic::PreparedTarget result;result.state=Design::ResolutionState::Changed;result.diagnostic="Target service closed";return result;},[binding,window=this->window,inputs=scene->inputs,id=stamp.resultId]{return binding && window && binding->current() && binding->stamp().resultId==id && Selection::EvaluationBinding::inputsCurrent(*inputs,*window);});publishReferences(service);document();
    }
  }
  refreshWidgets();
}
Design::DesignDocument *Controller::document(){
  if(!window || !window->activeEditor || !window->evaluationBinding->current())return nullptr;
  auto *document=Design::DesignDocument::forEditor(*window->activeEditor,*window);auto id=window->activeEditor->documentId();
  auto found=documents.find(id);if(found!=documents.end() && found->second.owner==document)return document;
  if(found!=documents.end())documents.erase(found);Document state;state.owner=document;
  for(const std::string kind:{"Anchor","Point","Axis","Plane","Attachment"}){
    Design::LifecycleCallbacks callbacks;
    callbacks.resolveConsumerRecord=[this,kind](const auto& key,const auto& stamp){if(!window || !window->evaluationBinding->validate(stamp))return std::shared_ptr<const Design::Record>();auto parsed=referenceKey(key);auto value=parsed?record(*parsed):nullptr;return value && value->kind==kind?value:nullptr;};
    callbacks.prepareInsert=[this](const auto& draft,const auto& stamp){return insert(draft,stamp);};
    callbacks.prepareEdit=[this](const auto& key,const auto& input,const auto& stamp){std::string error;auto parsed=referenceKey(key);auto values=changes(input,error);if(!parsed || !error.empty())return Design::PreparedEdit{{},{},error.empty()?"Invalid owner key":error};return prepareEdit(*parsed,values,stamp);};
    callbacks.locatePromotedRecord=[this](const auto& draft,const auto& stamp){if(!window || !window->evaluationBinding->validate(stamp))return std::shared_ptr<const Design::Record>();auto key=referenceKey(draft.key);return key?record(*key):nullptr;};
    callbacks.previewConsumerRecord=[](const auto& before,const auto& input,auto& error){auto values=changes(input,error);return error.empty()?applyReferenceChanges(before,values,error):nullptr;};
    auto adapter=document->registerLifecycleAdapter(kind,this,std::move(callbacks));if(adapter)state.adapters.push_back(std::move(adapter));
  }
  connect(document,&Design::DesignDocument::changed,this,&Controller::refreshWidgets);
  connect(document,&QObject::destroyed,this,[this,id]{documents.erase(id);});
  documents.emplace(id,std::move(state));return document;
}
ReferenceResolution Controller::resolve(const ReferenceKey& key)const{
  if(!window || !service || !window->evaluationBinding->current()){ReferenceResolution result;result.state=Design::ResolutionState::Changed;result.diagnostic="Evaluation is stale or unavailable";return result;}
  auto token=anchorToken(key);if(!token){ReferenceResolution result;result.state=Design::ResolutionState::Unsupported;result.diagnostic="Reference owner cannot be represented by a retained semantic target";return result;}
  const auto& stamp=window->evaluationBinding->stamp();return service->resolve(*token,stamp.resultId.toStdString(),stamp.sourceFingerprint.toStdString(),stamp.configurationFingerprint.toStdString());
}
std::shared_ptr<const Design::Record> Controller::record(const ReferenceKey& key)const{
  auto reference=resolve(key);if(reference.declaration)return reference.declaration;
  if(!window || !window->evaluationBinding->current())return {};auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());auto refs=scene && scene->semantic?referenceScene(*scene->semantic):nullptr;
  if(key.owner.role==OwnerRole::DefinitionDefault){auto segments=Semantic::pathSegments(key.owner.id);if(segments && !segments->empty()){auto prepared=window->semanticController->targetService()->resolve({Semantic::TargetRole::Definition,segments->front()});if(prepared.defaults && prepared.defaults->scene)refs=referenceScene(*prepared.defaults->scene);}}
  if(refs){auto attachment=refs->attachments.find(key);if(attachment!=refs->attachments.end())return attachment->second.declaration;}return {};
}
bool Controller::selectReference(ReferenceKey key){if(!record(key) && resolve(key).state==Design::ResolutionState::Changed)return false;selected=std::move(key);refreshWidgets();return true;}
std::shared_ptr<const ReferenceScene> Controller::listedReferences(){
  if(!window || !window->evaluationBinding->current())return {};auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());
  auto target=window->semanticController->selectedTarget();if(target && target->role==Semantic::TargetRole::Definition){auto prepared=window->semanticController->targetService()->resolve(*target);if(prepared.defaults && prepared.defaults->scene)return referenceScene(*prepared.defaults->scene);return {};}
  return scene && scene->semantic?referenceScene(*scene->semantic):nullptr;
}
std::optional<std::string> Controller::capture(const Selection::Hit& hit,std::optional<std::string> owner,std::string *error){
  auto fail=[&](const std::string& message)->std::optional<std::string>{if(error)*error=message;return {};};
  if(!window || !window->evaluationBinding->validate(hit.stamp) || hit.object.resultId!=hit.stamp.resultId)return fail("Capture requires a current surface hit");
  auto scene=window->evaluationBinding->scene(hit.stamp);if(!scene || !scene->semantic)return fail("Captured semantic context is unavailable");
  if(!owner)owner=window->semanticController->pathForDraw(hit.object.drawId).value_or("");
  Transform3d placement=Transform3d::Identity();if(!owner->empty()){auto found=scene->semantic->instances.find(*owner);if(found==scene->semantic->instances.end())return fail("Capture owner is missing or ambiguous");placement=found->second.localToWorld;}
  const auto cameraUp=CameraPose::matrix(window->qglview->cam.object_rot).transpose()*Vector3d::UnitZ();
  auto captured=captureAnchor(hit.worldPoint,hit.outwardNormal,cameraUp,placement);if(!captured.anchor)return fail(captured.diagnostic);
  cancelCapture();auto *doc=document();if(!doc)return fail("Capture document is unavailable");
  auto id="anchor-"+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();ReferenceKey key{owner->empty()?OwnerKey{OwnerRole::Document,scene->inputs->root().fileKey}:instanceOwner(*owner,""),id};
  captureToken=doc->createConsumerTransient("Anchor",opaqueKey(key),capturedDefinition(id,"Fixed anchor",*captured.anchor,owner->empty()?std::nullopt:owner),hit.stamp,error);
  refreshWidgets();return captureToken;
}
bool Controller::beginPick(){
  if(!window || !window->evaluationBinding->current())return false;window->inspectorDock->show();window->inspectorDock->raise();
  tool=window->viewportTools->beginPick({"Pick anchor",window->evaluationBinding->stamp(),true},this,[this](const Selection::PickResult& result){if(result.hit){std::string error;capture(*result.hit,{},&error);report(error);}else if(result.error!=Selection::PickError::Cancelled)report(result.detail.toStdString());});return tool!=0;
}
std::optional<Design::Draft> Controller::captureDraft()const{
  if(!window || !window->activeEditor)return {};auto doc=window->activeEditor->findChild<Design::DesignDocument *>(QString(),Qt::FindDirectChildrenOnly);if(!doc)return {};
  if(captureToken){auto found=doc->drafts().find(*captureToken);if(found!=doc->drafts().end())return found->second;}
  // Promotion undo restores the authoritative draft in 14 after this controller's transient token was retired.
  for(const auto& [token,draft]:doc->drafts())if(draft.consumer && draft.kind=="Anchor" && !draft.before->source)return draft;
  return {};
}
void Controller::cancelCapture(){auto draft=captureDraft();captureToken.reset();if(draft && window && window->activeEditor)if(auto doc=window->activeEditor->findChild<Design::DesignDocument *>(QString(),Qt::FindDirectChildrenOnly))doc->cancelDraft(draft->token);refreshWidgets();}
Design::LifecycleResult Controller::saveCapture(){auto draft=captureDraft();auto doc=document();if(!draft || !doc)return {false,"No current captured Anchor"};auto result=doc->promoteConsumer(draft->token);if(result)captureToken.reset();report(result.error);return result;}
size_t Controller::declarationUses(const Design::Record& record)const{if(!record.source || !window || !window->evaluationBinding->current())return 0;auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());auto refs=scene && scene->semantic?referenceScene(*scene->semantic):nullptr;if(!refs)return 0;auto found=refs->sourceUseCounts.find(sourceIdentity(*record.source));return found==refs->sourceUseCounts.end()?0:found->second;}
bool Controller::allowSharedEdit(const ReferenceKey& key){auto value=record(key);if(!value || declarationUses(*value)<=1)return false;sharedEdits.insert(key);refreshWidgets();return true;}
Design::PreparedEdit Controller::prepareEdit(const ReferenceKey& key,const std::vector<LiteralChange>& input,const Selection::Stamp& stamp)const{
  auto fail=[](const std::string& error){return Design::PreparedEdit{{},{},error};};
  if(!window || !window->evaluationBinding->validate(stamp))return fail("Evaluation is stale");auto value=record(key);if(!value || !value->source)return fail("Source declaration is unavailable");
  auto scene=window->evaluationBinding->scene(stamp);auto captured=scene->inputs->consumed().find(value->source->fileKey);if(captured==scene->inputs->consumed().end())return fail("Source was not captured");
  const auto& source=captured->second;EditorInterface *owner=nullptr;
  for(auto *editor:window->tabManager->editorList)if(source.editorId && source.editorRevision && editor->documentId().toStdString()==*source.editorId && editor->sourceRevision==*source.editorRevision && editor->sourceFingerprint.toStdString()==source.fingerprint && !editor->isSourceReadOnly()){if(owner)return fail("Source editor is ambiguous");owner=editor;}
  if(!owner)return fail("Open the writable source owner and reevaluate before editing");
  std::string error;auto patches=planReferenceEdit(*value,source,input,declarationUses(*value),sharedEdits.count(key)!=0,error);if(!patches)return fail(error);
  if(value->kind=="Attachment"){
    auto preview=applyReferenceChanges(*value,input,error);auto refs=referenceScene(*scene->semantic);auto relation=refs->attachments.at(key);relation.value=*std::dynamic_pointer_cast<const AttachmentDeclaration>(preview->evaluated);
    auto solved=validateAttachmentDraft(relation,*refs,scene->semantic->instances,key);if(!solved.valid())return fail(solved.graph.valid()?solved.diagnostics.front().message:solved.graph.diagnostics.front().message);
  }
  SourceEditing::EditRequest request{owner->documentId(),owner->sourceRevision,owner->sourceFingerprint,{}};for(const auto& patch:*patches)request.edits.append({qsizetype(patch.range.begin),qsizetype(patch.range.end),QByteArray::fromStdString(patch.expected),QByteArray::fromStdString(patch.replacement)});
  QVector<SourceEditing::ByteEdit> edits;auto check=SourceEditing::validate(owner->toPlainText().toUtf8(),owner->documentId(),owner->sourceRevision,owner->sourceFingerprint,owner->isSourceReadOnly(),request,edits);if(check.error!=SourceEditing::EditError::None)return fail(check.detail.toStdString());return {owner,std::move(request),{}};
}
Design::LifecycleResult Controller::editReference(const ReferenceKey& key,const std::vector<LiteralChange>& input){
  auto doc=document();auto value=record(key);if(!doc || !value)return {false,"Reference is unavailable"};auto draft=doc->beginConsumerEdit(value->kind,opaqueKey(key),window->evaluationBinding->stamp());if(!draft)return {false,"Reference already has a draft"};
  std::vector<Design::FieldChange> fields;for(const auto& change:input){Design::FieldChange field{Design::ChangeKind::LiteralField};field.fieldPath=change.path;field.literal=change.value;fields.push_back(std::move(field));}
  auto updated=doc->updateDraft(*draft,fields);if(!updated){doc->cancelDraft(*draft);return updated;}auto result=doc->commitDraft(*draft);if(!result)doc->cancelDraft(*draft);report(result.error);return result;
}
Design::PreparedEdit Controller::insert(const Design::Draft& draft,const Selection::Stamp& stamp)const{
  auto fail=[](const std::string& error){return Design::PreparedEdit{{},{},error};};if(!window || !window->activeEditor || !window->evaluationBinding->validate(stamp))return fail("Capture evaluation is stale");
  auto *editor=window->activeEditor;if(editor->isSourceReadOnly() || editor->documentId()!=stamp.documentId || draft.kind!="Anchor" || draft.preview->source)return fail("Save requires the open writable root document");
  auto scene=window->evaluationBinding->scene(stamp);auto key=referenceKey(draft.key);auto payload=std::dynamic_pointer_cast<const ReferenceDeclaration>(draft.preview->evaluated);if(!key || !payload || !std::holds_alternative<Anchor>(payload->value))return fail("Invalid captured definition");
  if(key->owner.role==OwnerRole::Instance){if(!payload->owner || *payload->owner!=key->owner.id || !scene->semantic->instances.count(key->owner.id))return fail("Capture owner changed");}
  else if(key->owner.role!=OwnerRole::Document || key->owner.id!=scene->inputs->root().fileKey || payload->owner)return fail("Capture document owner changed");
  auto refs=referenceScene(*scene->semantic);if(refs && (refs->references.count(*key) || refs->invalidReferences.count(*key)))return fail("Reference id collision");
  std::set<std::string> names;for(const auto& assignment:scene->backing->parsedRoot->scope->assignmentOccurrences)names.insert(assignment->getName());
  std::string base="anchor_";for(unsigned char c:draft.preview->id)base+=std::isalnum(c)?char(c):'_';std::string variable=base;for(unsigned ordinal=2;names.count(variable);++ordinal)variable=base+"_"+std::to_string(ordinal);
  auto declaration=serializeReference(*draft.preview,variable);if(!declaration)return fail("Captured Anchor cannot be serialized");auto bytes=editor->toPlainText().toUtf8();auto added=QByteArray::fromStdString(*declaration);if(bytes.contains("\r\n"))added.replace("\n","\r\n");if(!bytes.isEmpty() && !bytes.endsWith('\n'))added.prepend(bytes.contains("\r\n")?"\r\n":"\n");
  return {editor,{editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{{bytes.size(),bytes.size(),{},added}}},{}};
}
void Controller::navigate(const ReferenceKey& key){auto value=record(key);if(!window || !value || !value->source)return;auto scene=window->evaluationBinding->scene(window->evaluationBinding->stamp());if(!scene || !scene->provenance)return;const auto& source=*value->source;SourceProvenance::Site site;site.kind=SourceProvenance::SiteKind::Assignment;site.whole={source.fileKey,source.fingerprint,source.beginByte,source.endByte};site.head=site.whole;QString error;SourceProvenance::ProvenanceService::forWindow(window)->navigateSite(scene->provenance->syntaxTable(),site,window->evaluationBinding->stamp(),&error);report(error.toStdString());}
}
