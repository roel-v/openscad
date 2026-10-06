#include "core/anchors/ReferenceCollector.h"
#include "core/anchors/AttachmentSolver.h"
#include "core/semantic/SemanticCollector.h"
#include "core/EvaluationSession.h"
#include <algorithm>
#include <stdexcept>

namespace Anchors {
namespace {
struct Pending { std::shared_ptr<const Design::Record> record; Semantic::ScopeOwner scope; };
struct Collection : Design::Payload {
  std::vector<Pending> declarations;
  std::vector<Design::Diagnostic> diagnostics;
  bool invalidRelation = false;
};
std::shared_ptr<Collection> collection(EvaluationSession& session) {
  auto& state = session.consumerStates[extensionId];
  if (!state) state = std::make_shared<Collection>();
  return std::static_pointer_cast<Collection>(state);
}
void diagnostic(ReferenceScene& scene,const std::string& code,const std::string& message,const Design::Record& record) {
  scene.diagnostics.push_back({code,message,record.source});
}
OwnerKey ownerFor(const std::string& path,const Semantic::ScopeOwner& scope) {
  return path.empty() ? OwnerKey{OwnerRole::Document,scope.document} : instanceOwner(path,scope.defaultDefinitionId);
}
std::shared_ptr<ReferenceScene> finishCollection(const Collection& state, const Semantic::PlacementContext& context) {
  auto scene = std::make_shared<ReferenceScene>(); scene->diagnostics = state.diagnostics; scene->relationsValid = !state.invalidRelation;
  for (const auto& pending : state.declarations) {
    const auto& record = *pending.record; const auto& scope = pending.scope;
    scene->documentPath = scope.document; scene->defaultDefinitionId = scope.defaultDefinitionId;
    if (record.source) ++scene->sourceUseCounts[sourceIdentity(*record.source)];
    if (auto reference = std::dynamic_pointer_cast<const ReferenceDeclaration>(record.evaluated)) {
      auto path = reference->owner ? resolvePath(*reference->owner,scope.declaringAssemblyPath) : std::optional<std::string>(scope.instancePath);
      if (!path || (!path->empty() && !context.instances().count(*path))) {
        diagnostic(*scene,"anchor-owner","Missing or invalid reference owner",record); continue;
      }
      ReferenceKey key{ownerFor(*path,scope),record.id};
      ReferenceRecord result{key,record.label,reference->value,pending.record,scope.definitionId,scope.invocationId,bool(reference->owner)};
      auto previous = scene->references.find(key);
      if (scene->invalidReferences.count(key) || previous != scene->references.end()) {
        if (previous != scene->references.end()) {
          diagnostic(*scene,"duplicate-reference","Duplicate reference " + key.id + " in " + key.owner.id,*previous->second.declaration);
          scene->references.erase(previous);
        }
        scene->invalidReferences.insert(key); diagnostic(*scene,"duplicate-reference","Duplicate reference " + key.id + " in " + key.owner.id,record);
      } else scene->references.emplace(key,std::move(result));
    } else if (auto relation = std::dynamic_pointer_cast<const AttachmentDeclaration>(record.evaluated)) {
      ReferenceKey key{ownerFor(scope.instancePath,scope),record.id};
      auto childPath = resolvePath(relation->child.component,scope.instancePath);
      auto targetPath = resolvePath(relation->target.component,scope.instancePath);
      if (!childPath || childPath->empty() || !context.instances().count(*childPath) || !targetPath || (!targetPath->empty() && !context.instances().count(*targetPath))) {
        diagnostic(*scene,"attachment-owner","Attachment " + record.id + " has a missing or invalid Component path",record); scene->relationsValid = false; continue;
      }
      Attachment result{key,record.label,{{ownerFor(*childPath,scope),relation->child.anchor},*childPath},{{ownerFor(*targetPath,scope),relation->target.anchor},*targetPath},*relation,pending.record,scope.invocationId};
      auto previous = scene->attachments.find(key);
      if (scene->invalidAttachments.count(key) || previous != scene->attachments.end()) {
        if (previous != scene->attachments.end()) { diagnostic(*scene,"duplicate-attachment","Duplicate Attachment " + key.id,*previous->second.declaration); scene->attachments.erase(previous); }
        scene->invalidAttachments.insert(key); diagnostic(*scene,"duplicate-attachment","Duplicate Attachment " + key.id,record); scene->relationsValid = false;
      } else scene->attachments.emplace(key,std::move(result));
    }
  }
  std::map<OwnerKey,ReferenceKey> drawings;
  for (const auto& [key,reference] : scene->references) if (auto anchor = std::get_if<Anchor>(&reference.value); anchor && anchor->drawingFrame) {
    auto [found,inserted] = drawings.emplace(key.owner,key);
    if (!inserted) {
      diagnostic(*scene,"duplicate-drawing-frame","An owner permits one drawing Anchor",*reference.declaration);
      diagnostic(*scene,"duplicate-drawing-frame","An owner permits one drawing Anchor",*scene->references.at(found->second).declaration);
      scene->invalidReferences.insert(key); scene->invalidReferences.insert(found->second);
      scene->invalidDrawingOwners.insert(key.owner);
    }
  }
  for (const auto& key : scene->invalidReferences) scene->references.erase(key);
  for (const auto& [key,relation] : scene->attachments) {
    for (const auto& endpoint : {relation.child,relation.target}) {
      auto found = scene->references.find(endpoint.reference);
      if (found == scene->references.end() || !std::holds_alternative<Anchor>(found->second.value)) {
        diagnostic(*scene,"attachment-anchor","Attachment " + relation.key.id + " needs an oriented Anchor: " + endpoint.path + ":" + endpoint.reference.id,*relation.declaration); scene->relationsValid = false;
      }
    }
    auto [driver,inserted] = scene->drivers.emplace(relation.child.path,key);
    if (!inserted) {
      diagnostic(*scene,"attachment-driver","Multiple Attachments drive " + relation.child.path,*relation.declaration);
      diagnostic(*scene,"attachment-driver","Multiple Attachments drive " + relation.child.path,*scene->attachments.at(driver->second).declaration); scene->relationsValid = false;
    }
  }
  return scene;
}
}
std::optional<std::string> resolvePath(const std::string& authored,const std::string& relative) {
  if (authored.empty()) return relative;
  const auto path = authored.front() == '/' ? authored : relative + "/" + authored;
  auto segments = Semantic::pathSegments(path); if (!segments) return {};
  std::string canonical;
  for (const auto& segment : *segments) {
    if (segment == "." || segment == ".." || segment.find('*') != std::string::npos) return {};
    canonical += "/" + Semantic::escapeSegment(segment);
  }
  return canonical == path ? std::optional<std::string>(canonical) : std::nullopt;
}
void collectReference(Design::Record record,const Design::EvaluatedDeclaration& declaration,EvaluationSession& session) {
  auto state = collection(session);
  auto scope = Semantic::currentScope(session,declaration.nativeContext,declaration.localScope);
  bool permitted = scope && (scope->instancePath.empty() || scope->isDirectModuleBody);
  bool part = false;
  if (scope && !scope->definitionId.empty()) part = session.semanticCollector->finish()->definitions.at(scope->definitionId).kind == Semantic::DefinitionKind::Part;
  auto reference = std::dynamic_pointer_cast<const ReferenceDeclaration>(record.evaluated);
  if (part && (record.kind == "Attachment" || (reference && reference->owner))) permitted = false;
  if (!permitted) {
    state->diagnostics.push_back({"anchor-scope","Reference declarations require file/direct Part or Assembly scope; Part excludes Attachment and explicit owner",record.source});
    state->invalidRelation |= record.kind == "Attachment"; return;
  }
  state->declarations.push_back({std::make_shared<const Design::Record>(std::move(record)),*scope});
}
void rejectReference(const std::string& kind,const std::vector<Design::Diagnostic>& diagnostics,const Design::EvaluatedDeclaration&,EvaluationSession& session) {
  auto state = collection(session); state->diagnostics.insert(state->diagnostics.end(),diagnostics.begin(),diagnostics.end()); state->invalidRelation |= kind == "Attachment";
}
void registerCollectionPass() {
  static auto registration = Semantic::registerPlacementPass("anchor-placement",0,[](Semantic::PlacementContext& context) {
    auto state = std::dynamic_pointer_cast<Collection>(context.consumerState(extensionId));
    if (!state) return Semantic::PlacementOutcome{};
    auto scene = finishCollection(*state,context);
    if (!scene->relationsValid) return Semantic::PlacementOutcome::failure("attachment-invalid",scene->diagnostics.empty() ? "Invalid Attachment" : scene->diagnostics.back().message);
    auto solved = solveGraph(*scene,context.instances());
    if (!solved.valid()) {
      const auto& diagnostic = solved.graph.valid() ? solved.diagnostics.front() : solved.graph.diagnostics.front();
      return Semantic::PlacementOutcome::failure(diagnostic.code,diagnostic.message);
    }
    // Compute every candidate first. A failed solve never exposes an unplaced native tree.
    std::map<std::string,Transform3d> corrections;
    for (const auto& path : solved.graph.order) if (auto correction=solved.worldCorrections.find(path);correction!=solved.worldCorrections.end()) {
      const auto& instance=context.instances().at(path);
      Transform3d outside=solved.placements.at(instance.parentPath)*instance.originalLocalToParent;
      std::string error;auto inverse=supportedInverse(outside,error);
      if (!inverse) return Semantic::PlacementOutcome::failure("attachment-envelope",path+": "+error);
      Transform3d local=*inverse*correction->second*outside;
      if (!local.matrix().allFinite() || !(outside*local).matrix().isApprox((correction->second*outside).matrix(),1e-10))
        return Semantic::PlacementOutcome::failure("attachment-envelope",path+": Placement envelope exceeds the supported residual");
      corrections.emplace(path,std::move(local));
    }
    for (const auto& path : solved.graph.order) if (auto found=corrections.find(path);found!=corrections.end()) {
      auto outcome=context.insertTransform(path,found->second);if(!outcome.success)return outcome;
    }
    scene->placements=std::move(solved.placements);
    return context.publishExtension(extensionId,std::move(scene));
  });
  if (!registration) throw std::logic_error("Anchor placement pass is already registered");
}
}
