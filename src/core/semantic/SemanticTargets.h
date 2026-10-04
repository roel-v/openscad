#pragma once
#include "core/semantic/SemanticEvaluation.h"
#include "core/design/DesignReferences.h"
#include <mutex>
class Geometry;
namespace Semantic {
enum class TargetRole { Definition, Instance };
struct TargetKey {
  TargetRole role;
  std::string id;
  bool operator<(const TargetKey& other) const { return role != other.role ? role < other.role : id < other.id; }
  bool operator==(const TargetKey& other) const { return role == other.role && id == other.id; }
};
std::optional<TargetKey> targetKey(const Design::ReferenceToken&, std::string *error = nullptr);
Design::ReferenceToken targetToken(TargetKey);
struct PreparedTarget {
  Design::ResolutionState state = Design::ResolutionState::Invalid;
  std::string resultId, diagnostic;
  TargetKey key{TargetRole::Instance, ""};
  std::shared_ptr<const Scene> scene;
  std::shared_ptr<const AbstractNode> root;
  std::shared_ptr<const DefaultEvaluation> defaults;
  std::map<std::string, Parameter> parameters;
  Transform3d localToWorld = Transform3d::Identity();
  std::shared_ptr<const Geometry> localGeometry, placedGeometry;
  std::optional<BoundingBox> localBounds, worldBounds;
};
PreparedTarget prepareTarget(std::shared_ptr<const Scene>, TargetKey, std::string resultId,
                            std::string documentPath, const std::function<void(Context&)>& configure = {},
                            const std::function<bool()>& cancelled = {});
// A service owns one current result and asynchronously supplies prepared immutable target entries.
class Targets {
public:
  Targets(std::string resultId, std::shared_ptr<const Scene>, std::function<void(TargetKey)> request);
  PreparedTarget resolve(const Design::ReferenceToken&, const std::string& expectedResult);
  bool install(std::shared_ptr<const PreparedTarget>);
  void cancelPending();
  void clearTarget(const TargetKey&);
  const std::string& resultId() const { return result; }
private:
  std::string result;
  std::shared_ptr<const Scene> scene;
  std::function<void(TargetKey)> request;
  std::mutex mutex;
  std::map<TargetKey, std::shared_ptr<const PreparedTarget>> ready;
  std::set<TargetKey> pending;
};
// The domain resolver selects the exact result ID supplied by 14's existing ResolveContext.
void publishTargets(const std::shared_ptr<Targets>&);
void removeTargets(const std::string& resultId);
}
