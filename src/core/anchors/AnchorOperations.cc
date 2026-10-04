#include "core/anchors/AnchorOperations.h"
#include "core/EvaluationInputs.h"
namespace Anchors {
namespace {
std::optional<Transform3d> ownerTransform(const Semantic::Scene& scene,const OwnerKey& owner) {
  auto references=referenceScene(scene);
  if(owner.role==OwnerRole::Document) {
    if(references && owner.id==references->documentPath)return Transform3d::Identity();
    if(scene.inputs && owner.id==scene.inputs->root().fileKey)return Transform3d::Identity();
    return {};
  }
  if((owner.role==OwnerRole::DefinitionDefault)!=!scene.defaultDefinitionId.empty())return {};
  auto found=scene.instances.find(owner.id);return found==scene.instances.end()?std::nullopt:std::optional<Transform3d>(found->second.localToWorld);
}
}
ReferenceResolution resolveReference(std::shared_ptr<const Semantic::Scene> scene,const ReferenceKey& key) {
  ReferenceResolution out;out.retainedScene=scene;
  auto fail=[&](Design::ResolutionState state,const std::string& message){out.state=state;out.diagnostic=message;return out;};
  if(!scene || !scene->valid())return fail(Design::ResolutionState::Invalid,"Semantic evaluation is invalid");
  out.retainedReferences=referenceScene(*scene);
  if(!out.retainedReferences)return fail(Design::ResolutionState::Missing,"The evaluated source has no named references");
  if(out.retainedReferences->invalidReferences.count(key))return fail(Design::ResolutionState::Invalid,"Duplicate or invalid reference: "+key.owner.id+":"+key.id);
  auto owner=ownerTransform(*scene,key.owner);if(!owner)return fail(Design::ResolutionState::Missing,"Reference owner is missing: "+key.owner.id);
  auto found=out.retainedReferences->references.find(key);if(found==out.retainedReferences->references.end())return fail(Design::ResolutionState::Missing,"Named reference is missing: "+key.id);
  const auto& record=found->second;auto world=transformReference(record.value,*owner);
  out.declaration=record.declaration;out.local=record.value;out.reflectedOwner=world.reflectedOwner;
  if(!world)return fail(world.state,world.diagnostic);
  out.world=std::move(world.worldValue);out.frame=world.frame;out.state=Design::ResolutionState::Resolved;
  if(auto anchor=std::get_if<Anchor>(&*out.world))out.bounds=BoundingBox(anchor->position,anchor->position);
  else if(auto point=std::get_if<Point>(&*out.world))out.bounds=BoundingBox(point->position,point->position);
  return out;
}
Design::Resolution canonicalFrame(const Semantic::Scene& scene,const OwnerKey& owner) {
  if(!scene.valid())return {Design::ResolutionState::Invalid,{}, {},"Semantic evaluation is invalid"};
  auto references=referenceScene(scene);
  if(references && references->invalidDrawingOwners.count(owner))return {Design::ResolutionState::Invalid,{}, {},"Owner has duplicate canonical drawing Anchors"};
  if(references)for(const auto& [key,record]:references->references)if(key.owner==owner) {
    if(auto anchor=std::get_if<Anchor>(&record.value);anchor && anchor->drawingFrame) {
      auto transform=ownerTransform(scene,owner);if(!transform)return {Design::ResolutionState::Missing,{}, {},"Drawing owner is missing"};
      auto result=transformReference(record.value,*transform);return {result.state,result.frame,{},result.diagnostic};
    }
  }
  auto transform=ownerTransform(scene,owner);
  if(!transform)return {Design::ResolutionState::Missing,{}, {},"Canonical frame owner is missing"};
  if(!properRigid(*transform))return {Design::ResolutionState::Unsupported,{}, {},"Non-rigid owner requires an explicit drawing Anchor"};
  return {Design::ResolutionState::Resolved,*transform,{}, {}};
}
SolvedGraph validateAttachmentDraft(const Attachment& draft,const ReferenceScene& current,const std::map<std::string,Semantic::Instance>& instances,std::optional<ReferenceKey> replacing) {
  ReferenceScene candidate=current;
  auto fail=[&](const std::string& message){SolvedGraph out;out.diagnostics.push_back({"attachment-draft",message,draft.declaration?draft.declaration->source:std::nullopt});return out;};
  if(!current.relationsValid || !current.invalidAttachments.empty())return fail("Current relation set is invalid");
  if(replacing) {
    if(!candidate.attachments.erase(*replacing))return fail("Replaced attachment is missing");
    if(!(draft.key==*replacing))return fail("An edit must preserve the Attachment identity");
  }
  if(candidate.attachments.count(draft.key))return fail("Attachment id already exists in its declaring owner");
  candidate.attachments.emplace(draft.key,draft);return solveGraph(candidate,instances);
}
}
