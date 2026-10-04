#include "core/anchors/ReferenceScene.h"
namespace Anchors {
std::shared_ptr<const ReferenceScene> referenceScene(const Semantic::Scene& scene) {
  auto found = scene.extensions.find(extensionId);
  return found == scene.extensions.end() ? nullptr : std::dynamic_pointer_cast<const ReferenceScene>(found->second);
}
std::string sourceIdentity(const Design::DeclarationKey& key) {
  return key.fileKey + ":" + std::to_string(key.beginByte) + ":" + std::to_string(key.endByte);
}
OwnerKey instanceOwner(const std::string& path, const std::string& defaults) {
  return {defaults.empty() ? OwnerRole::Instance : OwnerRole::DefinitionDefault, path};
}
}
