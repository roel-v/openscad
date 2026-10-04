#pragma once
#include "core/anchors/AttachmentSolver.h"
namespace Anchors {
struct ReferenceResolution {
  Design::ResolutionState state = Design::ResolutionState::Invalid;
  std::optional<ReferenceValue> local, world;
  std::optional<Transform3d> frame;
  std::optional<BoundingBox> bounds;
  std::shared_ptr<const Design::Record> declaration;
  std::shared_ptr<const ReferenceScene> retainedReferences;
  std::shared_ptr<const Semantic::Scene> retainedScene;
  bool reflectedOwner = false;
  std::string diagnostic;
};
ReferenceResolution resolveReference(std::shared_ptr<const Semantic::Scene>, const ReferenceKey&);
Design::Resolution canonicalFrame(const Semantic::Scene&, const OwnerKey&);
SolvedGraph validateAttachmentDraft(const Attachment&, const ReferenceScene&,
                                   const std::map<std::string,Semantic::Instance>&,
                                   std::optional<ReferenceKey> replacing = {});
}
