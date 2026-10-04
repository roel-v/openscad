#pragma once
#include "core/anchors/AnchorOperations.h"
#include "core/semantic/SemanticTargets.h"
#include <atomic>
namespace Anchors {
std::optional<Design::ReferenceToken> anchorToken(const ReferenceKey&);
class References {
public:
  using DefaultTarget = std::function<Semantic::PreparedTarget(Semantic::TargetKey)>;
  using Freshness = std::function<bool()>;
  References(std::string resultId,std::shared_ptr<const Semantic::Scene>,std::string inputsFingerprint,
             std::string configurationFingerprint,DefaultTarget = {},Freshness = {});
  ReferenceResolution resolve(const Design::ReferenceToken&,const std::string& expectedResult,
                              const std::string& inputsFingerprint,const std::string& configurationFingerprint) const;
  const std::string& resultId() const {return result;}
  void invalidate() {fresh.store(false);}
private:
  std::string result,inputs,configuration;
  std::shared_ptr<const Semantic::Scene> scene;
  DefaultTarget defaultTarget;
  Freshness inputsCurrent;
  std::atomic<bool> fresh{true};
};
void publishReferences(const std::shared_ptr<References>&);
void removeReferences(const std::string& resultId);
}
