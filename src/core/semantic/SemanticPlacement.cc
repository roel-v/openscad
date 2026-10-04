#include "core/semantic/SemanticPlacement.h"
#include "core/semantic/SemanticCollector.h"
#include "core/EvaluationSession.h"
#include "core/ModuleInstantiation.h"
#include "core/TransformNode.h"
#include "core/provenance/ProvenanceCollector.h"
#include <algorithm>
#include <mutex>
#include <stdexcept>
namespace Semantic {
namespace {
struct Entry { int order; PlacementPass callback; };
std::mutex passMutex;
std::map<std::string, Entry> passes;
void remove(const std::string& id) { std::lock_guard<std::mutex> lock(passMutex); passes.erase(id); }
}
PlacementRegistration::~PlacementRegistration() { if (!id.empty()) remove(id); }
PlacementRegistration::PlacementRegistration(PlacementRegistration&& other) noexcept : id(std::move(other.id)) { other.id.clear(); }
PlacementRegistration& PlacementRegistration::operator=(PlacementRegistration&& other) noexcept {
  if (this != &other) { if (!id.empty()) remove(id); id = std::move(other.id); other.id.clear(); } return *this;
}
PlacementRegistration registerPlacementPass(std::string id, int order, PlacementPass callback) {
  if (id.empty() || !callback) return {};
  std::lock_guard<std::mutex> lock(passMutex);
  if (!passes.emplace(id, Entry{order, std::move(callback)}).second) return {};
  return PlacementRegistration(std::move(id));
}
PlacementOutcome runPlacementPasses(PlacementContext& context) {
  std::vector<std::pair<std::string, Entry>> ordered;
  { std::lock_guard<std::mutex> lock(passMutex); for (const auto& item : passes) ordered.push_back(item); }
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    return a.second.order != b.second.order ? a.second.order < b.second.order : a.first < b.first;
  });
  for (const auto& [id, entry] : ordered) {
    try { auto result = entry.callback(context); if (!result.success) return result; }
    catch (const std::exception& error) { return PlacementOutcome::failure("placement-exception", id + ": " + error.what()); }
    catch (...) { return PlacementOutcome::failure("placement-exception", id + ": unknown exception"); }
  }
  return {};
}
std::optional<ScopeOwner> currentScope(const EvaluationSession& session, const Context *context, const LocalScope *scope) {
  return session.semanticCollector ? session.semanticCollector->currentScope(context, scope) : std::nullopt;
}
PlacementContext::PlacementContext(EvaluationSession& session, Scene& scene, std::shared_ptr<AbstractNode> root)
  : session(session), scene(scene), evaluatedRoot(std::move(root)) {}
PlacementOutcome PlacementContext::insertTransform(const std::string& path, const Transform3d& correction) {
  auto found = scene.instances.find(path);
  if (found == scene.instances.end() || !found->second.localRoot)
    return PlacementOutcome::failure("placement-instance", "Unknown Component: " + path);
  if (!correction.matrix().allFinite()) return PlacementOutcome::failure("placement-matrix", "Placement correction must be finite");
  const auto& instance = found->second;
  const auto *group = instance.localRoot.get();
  auto tagged = find_root_tag(evaluatedRoot);
  // Native root extraction would skip the new wrapper at or below this Component.
  std::function<bool(const AbstractNode&)> contains = [&](const AbstractNode& node) {
    if (&node == tagged.get()) return true;
    for (const auto& child : node.children) if (contains(*child)) return true;
    return false;
  };
  if (tagged && contains(*group)) return PlacementOutcome::failure("unsupported-placement-root", "The root modifier discards a required placement envelope; use tree Solo or retain the complete envelope");
  size_t replacements = 0;
  std::function<void(AbstractNode&)> wrap = [&](AbstractNode& parent) {
    for (auto& child : parent.children) {
      if (child.get() == group) {
        auto call = std::make_shared<ModuleInstantiation>("transform", AssignmentList{}, group->modinst->location());
        auto transform = std::make_shared<TransformNode>(call.get(), "semantic placement");
        transform->matrix = correction; transform->children.push_back(child); child = transform;
        scene.syntheticCalls.push_back(std::move(call));
        if (session.provenanceCollector) session.provenanceCollector->linkEmitted(transform->index(), instance.invocationId);
        ++replacements;
      } else wrap(*child);
    }
  };
  wrap(*evaluatedRoot);
  if (replacements != 1) return PlacementOutcome::failure("placement-topology", "Component must have one owned native group: " + path);
  return {};
}
}
