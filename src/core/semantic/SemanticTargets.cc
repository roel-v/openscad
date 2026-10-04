#include "core/semantic/SemanticTargets.h"
#include "core/NativeExecutionGuard.h"
#include "core/Tree.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/Geometry.h"
#include <mutex>
namespace Semantic {
namespace {
std::mutex registryMutex;
std::map<std::string, std::weak_ptr<Targets>> registry;
std::once_flag registration;
PreparedTarget failure(Design::ResolutionState state, std::string message) { PreparedTarget result; result.state = state; result.diagnostic = std::move(message); return result; }
}
std::optional<TargetKey> targetKey(const Design::ReferenceToken& token, std::string *error) {
  auto fail = [&](const std::string& message) -> std::optional<TargetKey> { if (error) *error = message; return {}; };
  if (token.domain != "semantic" || token.id.empty() || token.data.type() != Value::Type::OBJECT) return fail("Semantic target requires a domain, id and role object");
  // Feature 14 retains the complete native reference object in ReferenceToken::data.
  const auto& reference = token.data.toObject();
  if (reference.keys().size() != 3 || reference.get("domain").type() != Value::Type::STRING ||
      reference.get("domain").toString() != token.domain || reference.get("id").type() != Value::Type::STRING ||
      reference.get("id").toString() != token.id || reference.get("data").type() != Value::Type::OBJECT)
    return fail("Semantic reference requires only matching domain, id and data fields");
  const auto& data = reference.get("data").toObject();
  if (data.keys().size() != 1 || !data.contains("role") || data.get("role").type() != Value::Type::STRING) return fail("Semantic data requires only role");
  const auto role = data.get("role").toString();
  if (role == "definition") return TargetKey{TargetRole::Definition, token.id};
  if (role != "instance") return fail("Unknown semantic target role");
  auto segments = pathSegments(token.id); if (!segments || segments->empty()) return fail("Semantic instance id must be an encoded full path");
  std::string canonical; for (const auto& segment : *segments) canonical += "/" + escapeSegment(segment);
  if (canonical != token.id) return fail("Semantic instance path is not canonical");
  return TargetKey{TargetRole::Instance, token.id};
}
Design::ReferenceToken targetToken(TargetKey key) {
  ObjectType data(nullptr); data.set("role", Value(key.role == TargetRole::Definition ? "definition" : "instance"));
  ObjectType reference(nullptr); reference.set("domain", Value("semantic")); reference.set("id", Value(key.id));
  reference.set("data", Value(std::move(data)));
  return {"semantic", std::move(key.id), Value(std::move(reference))};
}
PreparedTarget prepareTarget(std::shared_ptr<const Scene> scene, TargetKey key, std::string resultId,
                            std::string path, const std::function<void(Context&)>& configure,
                            const std::function<bool()>& cancelled) {
  auto failure = [scene, key, resultId](Design::ResolutionState state, std::string message) {
    PreparedTarget result; result.state = state; result.diagnostic = std::move(message);
    result.scene = scene; result.key = key; result.resultId = resultId; return result;
  };
  auto stopped = [&] { return cancelled && cancelled(); };
  if (!scene || !scene->valid()) return failure(Design::ResolutionState::Invalid, "Semantic scene is invalid");
  if (stopped()) return failure(Design::ResolutionState::Unsupported, "Target geometry cancelled");
  NativeExecutionGuard execution;
  if (stopped()) return failure(Design::ResolutionState::Unsupported, "Target geometry cancelled");
  PreparedTarget output; output.scene = scene; output.resultId = std::move(resultId); output.key = key;
  try {
    if (key.role == TargetRole::Definition) {
      if (!scene->definitions.count(key.id)) return failure(Design::ResolutionState::Missing, "Missing semantic definition: " + key.id);
      auto defaults = std::make_shared<DefaultEvaluation>(evaluateDefaults(*scene, key.id, configure));
      if (!defaults->valid()) return failure(Design::ResolutionState::Invalid, defaults->diagnostic);
      const auto& instance = defaults->scene->instances.at("/" + escapeSegment(key.id));
      output.root = instance.localRoot; output.parameters = instance.parameters; output.defaults = std::move(defaults);
    } else {
      auto found = scene->instances.find(key.id);
      if (found == scene->instances.end()) return failure(Design::ResolutionState::Missing, "Missing semantic instance: " + key.id);
      output.root = found->second.localRoot; output.localToWorld = found->second.localToWorld; output.parameters = found->second.parameters;
    }
    if (!output.root) return failure(Design::ResolutionState::Invalid, "Target has no owned native root");
    Tree localTree(output.root, path); GeometryEvaluator localEvaluator(localTree);
    output.localGeometry = localEvaluator.evaluateGeometry(*output.root, true);
    if (stopped()) return failure(Design::ResolutionState::Unsupported, "Target geometry cancelled");
    if (output.localGeometry) output.localBounds = output.localGeometry->getBoundingBox();
    // Explicit target delivery retains its shape regardless of outer preview roles.
    // Transform an owned native geometry copy, preserving the immutable local result.
    if (output.localGeometry) {
      auto placed = std::shared_ptr<Geometry>(output.localGeometry->copy());
      placed->transform(output.localToWorld); output.placedGeometry = std::move(placed);
    }
    if (output.placedGeometry) output.worldBounds = output.placedGeometry->getBoundingBox();
    if (stopped()) return failure(Design::ResolutionState::Unsupported, "Target geometry cancelled");
    output.state = Design::ResolutionState::Resolved;
  } catch (const std::exception& error) { return failure(Design::ResolutionState::Invalid, error.what()); }
  return output;
}
Targets::Targets(std::string resultId, std::shared_ptr<const Scene> value, std::function<void(TargetKey)> request)
  : result(std::move(resultId)), scene(std::move(value)), request(std::move(request)) {}
PreparedTarget Targets::resolve(const Design::ReferenceToken& token, const std::string& expected) {
  std::string error; auto key = targetKey(token, &error); if (!key) return failure(Design::ResolutionState::Invalid, error);
  if (expected != result) return failure(Design::ResolutionState::Changed, "Semantic result changed; reevaluate the reference");
  if (!scene || !scene->valid()) return failure(Design::ResolutionState::Invalid, "Semantic scene is invalid");
  if (key->role == TargetRole::Definition ? !scene->definitions.count(key->id) : !scene->instances.count(key->id))
    return failure(Design::ResolutionState::Missing, "Missing semantic target: " + key->id);
  bool start = false;
  { std::lock_guard<std::mutex> lock(mutex); auto found = ready.find(*key); if (found != ready.end()) return *found->second; start = pending.insert(*key).second; }
  if (start && request) request(*key);
  return failure(Design::ResolutionState::Unsupported, "Preparing target geometry");
}
bool Targets::install(std::shared_ptr<const PreparedTarget> target) {
  if (!target || target->resultId != result) return false;
  std::lock_guard<std::mutex> lock(mutex); ready[target->key] = std::move(target); return true;
}
void Targets::cancelPending() {
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto& key : pending) if (!ready.count(key)) {
    auto target = std::make_shared<PreparedTarget>(); target->key = key; target->resultId = result;
    target->state = Design::ResolutionState::Unsupported; target->diagnostic = "Target geometry cancelled";
    ready.emplace(key, std::move(target));
  }
}
void Targets::clearTarget(const TargetKey& key) { std::lock_guard<std::mutex> lock(mutex); ready.erase(key); pending.erase(key); }
void publishTargets(const std::shared_ptr<Targets>& targets) {
  std::call_once(registration, [] {
    if (!Design::registerResolver("semantic", [](const Design::ReferenceToken& token, Design::ResolveContext& context) {
      std::shared_ptr<Targets> targets;
      { std::lock_guard<std::mutex> lock(registryMutex); auto found = registry.find(context.resultId); if (found != registry.end()) targets = found->second.lock(); }
      if (!targets) return Design::Resolution{Design::ResolutionState::Changed, {}, {}, "Semantic result is unavailable"};
      auto target = targets->resolve(token, context.resultId);
      return Design::Resolution{target.state, target.state == Design::ResolutionState::Resolved ? std::optional<Transform3d>(target.localToWorld) : std::nullopt, target.worldBounds, target.diagnostic};
    })) throw std::logic_error("Semantic reference domain is already registered");
  });
  if (targets) { std::lock_guard<std::mutex> lock(registryMutex); registry[targets->resultId()] = targets; }
}
void removeTargets(const std::string& result) { std::lock_guard<std::mutex> lock(registryMutex); registry.erase(result); }
}
