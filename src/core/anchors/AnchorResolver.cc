#include "core/anchors/AnchorResolver.h"
#include "core/design/DesignKinds.h"
#include <mutex>
#include <stdexcept>
namespace Anchors {
namespace {
std::mutex registryMutex;
std::map<std::string,std::weak_ptr<References>> registry;
std::once_flag registration;
ReferenceResolution failure(Design::ResolutionState state,const std::string& diagnostic) {ReferenceResolution out;out.state=state;out.diagnostic=diagnostic;return out;}
bool fields(const ObjectType& object,const std::set<std::string>& allowed) {
  if(!object.literalDuplicateKeys().empty())return false;
  for(const auto& key:object.keys())if(!allowed.count(key))return false;return true;
}
std::optional<Design::ReferenceToken> ownerToken(const Design::ReferenceToken& token,std::string& error) {
  auto fail=[&](const std::string& text)->std::optional<Design::ReferenceToken>{error=text;return {};};
  if(token.domain!="anchor" || token.id.empty() || token.data.type()!=Value::Type::OBJECT)return fail("Anchor token requires domain, nonempty id and owned reference data");
  const auto& object=token.data.toObject();
  if(!fields(object,{"domain","id","data"}) || object.keys().size()!=3 || object.get("domain").type()!=Value::Type::STRING || object.get("domain").toString()!=token.domain || object.get("id").type()!=Value::Type::STRING || object.get("id").toString()!=token.id || object.get("data").type()!=Value::Type::OBJECT)return fail("Anchor token requires only matching domain, id and data");
  const auto& data=object.get("data").toObject();
  if(!fields(data,{"owner"}) || data.keys().size()!=1)return fail("Anchor token data requires only owner");
  auto owner=Design::referenceToken(data.get("owner"),"");if(!owner)return fail("Anchor owner must be a document or semantic token");
  if(owner->domain=="document") {
    if(owner->data.type()!=Value::Type::OBJECT || !fields(owner->data.toObject(),{"domain","id"}) || !owner->id.empty())return fail("Document owner requires only document domain and an optional empty id");
  } else if(owner->domain!="semantic")return fail("Anchor owner must use the document or semantic domain");
  return owner;
}
}
std::optional<Design::ReferenceToken> anchorToken(const ReferenceKey& key) {
  if(key.id.empty())return {};
  Value owner=Value::undefined.clone();
  if(key.owner.role==OwnerRole::Document){ObjectType object(nullptr);object.set("domain",Value("document"));owner=Value(std::move(object));}
  else if(key.owner.role==OwnerRole::Instance)owner=Semantic::targetToken({Semantic::TargetRole::Instance,key.owner.id}).data;
  else {
    auto segments=Semantic::pathSegments(key.owner.id);if(!segments || segments->size()!=1)return {};
    owner=Semantic::targetToken({Semantic::TargetRole::Definition,segments->front()}).data;
  }
  ObjectType data(nullptr);data.set("owner",std::move(owner));ObjectType token(nullptr);token.set("domain",Value("anchor"));token.set("id",Value(key.id));token.set("data",Value(std::move(data)));
  return Design::ReferenceToken{"anchor",key.id,Value(std::move(token))};
}
References::References(std::string id,std::shared_ptr<const Semantic::Scene> value,std::string fingerprint,std::string config,DefaultTarget defaults,Freshness current)
  :result(std::move(id)),inputs(std::move(fingerprint)),configuration(std::move(config)),scene(std::move(value)),defaultTarget(std::move(defaults)),inputsCurrent(std::move(current)) {}
ReferenceResolution References::resolve(const Design::ReferenceToken& token,const std::string& expected,const std::string& fingerprint,const std::string& config) const {
  if(!fresh.load() || expected!=result || fingerprint!=inputs || config!=configuration || (inputsCurrent && !inputsCurrent()))return failure(Design::ResolutionState::Changed,"Anchor evaluation inputs or configuration changed");
  if(!scene || !scene->valid())return failure(Design::ResolutionState::Invalid,"Anchor evaluation is invalid");
  std::string error;auto owner=ownerToken(token,error);if(!owner)return failure(Design::ResolutionState::Invalid,error);
  auto active=scene;OwnerKey key;
  if(owner->domain=="document") {
    auto references=referenceScene(*active);if(!references)return failure(Design::ResolutionState::Missing,"Document has no named references");key={OwnerRole::Document,references->documentPath};
  } else {
    auto target=Semantic::targetKey(*owner,&error);if(!target)return failure(Design::ResolutionState::Invalid,error);
    if(target->role==Semantic::TargetRole::Definition) {
      if(!scene->definitions.count(target->id))return failure(Design::ResolutionState::Missing,"Missing reference definition: "+target->id);
      if(!defaultTarget)return failure(Design::ResolutionState::Unsupported,"Definition references require an owned prepared default evaluation");
      auto prepared=defaultTarget(*target);
      if(prepared.state!=Design::ResolutionState::Resolved)return failure(prepared.state,prepared.diagnostic);
      if(prepared.resultId!=result || !prepared.defaults || !prepared.defaults->valid() || prepared.defaults->scene->defaultDefinitionId!=target->id)return failure(Design::ResolutionState::Invalid,"Default target does not own the requested definition evaluation");
      active=prepared.defaults->scene;key=instanceOwner("/"+Semantic::escapeSegment(target->id),target->id);
    } else key=instanceOwner(target->id,active->defaultDefinitionId);
  }
  return resolveReference(std::move(active),{key,token.id});
}
void publishReferences(const std::shared_ptr<References>& references) {
  std::call_once(registration,[] {
    if(!Design::registerResolver("anchor",[](const Design::ReferenceToken& token,Design::ResolveContext& context) {
      std::shared_ptr<References> references;
      {std::lock_guard<std::mutex> lock(registryMutex);auto found=registry.find(context.resultId);if(found!=registry.end())references=found->second.lock();}
      if(!references)return Design::Resolution{Design::ResolutionState::Changed,{}, {},"Anchor result is unavailable"};
      auto resolved=references->resolve(token,context.resultId,context.evaluatedInputsFingerprint,context.configurationFingerprint);
      if(resolved.state==Design::ResolutionState::Resolved && !resolved.frame)return Design::Resolution{Design::ResolutionState::Unsupported,{},resolved.bounds,"Reference kind does not supply a rigid Anchor frame"};
      return Design::Resolution{resolved.state,resolved.frame,resolved.bounds,resolved.diagnostic};
    }))throw std::logic_error("Anchor domain is already registered");
  });
  if(references){std::lock_guard<std::mutex> lock(registryMutex);registry[references->resultId()]=references;}
}
void removeReferences(const std::string& id) {
  std::lock_guard<std::mutex> lock(registryMutex);auto found=registry.find(id);if(found!=registry.end()){if(auto references=found->second.lock())references->invalidate();registry.erase(found);}
}
}
