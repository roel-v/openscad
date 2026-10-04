#pragma once
#include "core/design/DesignObjects.h"
#include "glview/Camera.h"

namespace Design {
struct ResolveContext {
  std::string resultId, evaluatedInputsFingerprint, configurationFingerprint;
  BoundingBox documentBounds;
  const Records& records;
  std::vector<std::string> visitedIds;
};
using Resolver = std::function<Resolution(const ReferenceToken&, ResolveContext&)>;
bool registerResolver(std::string domain, Resolver);
Resolution resolve(const ReferenceToken&, ResolveContext&);
Resolution sectionFrame(const Record&, ResolveContext&);
std::optional<Camera> viewCamera(const Record&, ResolveContext&, std::string *diagnostic = nullptr);
struct ClosurePlan {
  std::vector<std::shared_ptr<const Record>> insert;
  std::vector<std::shared_ptr<const Record>> sourced;
  std::vector<Diagnostic> diagnostics;
  bool valid() const { return diagnostics.empty(); }
};
ClosurePlan persistenceClosure(const std::vector<std::string>& requested, const Records&,
                              const std::set<std::string>& includedFiles,
                              const std::set<std::string>& destinationVariables = {});
std::string declarationName(const Record&);
std::string serialize(const Record&);
}
