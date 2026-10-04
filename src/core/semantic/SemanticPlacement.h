#pragma once
#include "core/semantic/SemanticScene.h"
#include <functional>
class EvaluationSession;
class Context;
class LocalScope;
namespace Semantic {
struct ScopeOwner {
  std::string document, definitionId, instancePath, declaringAssemblyPath;
  uint64_t invocationId = 0;
  bool isDirectModuleBody = false;
};
std::optional<ScopeOwner> currentScope(const EvaluationSession&, const Context *, const LocalScope *);
struct PlacementOutcome {
  bool success = true;
  std::string code, message;
  static PlacementOutcome failure(std::string code, std::string message) {
    return {false, std::move(code), std::move(message)};
  }
};
class PlacementContext {
public:
  std::shared_ptr<const AbstractNode> root() const { return evaluatedRoot; }
  const std::map<std::string, Instance>& instances() const { return scene.instances; }
  const std::map<std::string, Definition>& definitions() const { return scene.definitions; }
  // A correction in the Component's local frame, outside its returned module group.
  // Authored envelope M followed by this K becomes M*K. No inverse is computed here.
  PlacementOutcome insertTransform(const std::string& instancePath, const Transform3d& correction);
private:
  friend class Collector;
  PlacementContext(EvaluationSession&, Scene&, std::shared_ptr<AbstractNode>);
  EvaluationSession& session;
  Scene& scene;
  std::shared_ptr<AbstractNode> evaluatedRoot;
};
using PlacementPass = std::function<PlacementOutcome(PlacementContext&)>;
class PlacementRegistration {
public:
  PlacementRegistration() = default;
  ~PlacementRegistration();
  PlacementRegistration(PlacementRegistration&&) noexcept;
  PlacementRegistration& operator=(PlacementRegistration&&) noexcept;
  PlacementRegistration(const PlacementRegistration&) = delete;
  PlacementRegistration& operator=(const PlacementRegistration&) = delete;
  explicit operator bool() const { return !id.empty(); }
private:
  friend PlacementRegistration registerPlacementPass(std::string, int, PlacementPass);
  explicit PlacementRegistration(std::string value) : id(std::move(value)) {}
  std::string id;
};
PlacementRegistration registerPlacementPass(std::string id, int order, PlacementPass);
PlacementOutcome runPlacementPasses(PlacementContext&);
}
