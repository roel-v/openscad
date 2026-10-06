#pragma once
#include "core/anchors/ReferenceScene.h"
namespace Anchors {
struct FrameResult {
  Design::ResolutionState state = Design::ResolutionState::Unsupported;
  std::optional<Transform3d> frame;
  std::optional<ReferenceValue> worldValue;
  bool reflectedOwner = false;
  std::string diagnostic;
  explicit operator bool() const { return state == Design::ResolutionState::Resolved; }
};
bool properRigid(const Transform3d&);
std::optional<Transform3d> supportedInverse(const Transform3d&, std::string& error);
FrameResult localFrame(const Anchor&);
FrameResult transformReference(const ReferenceValue&, const Transform3d& owner);
struct CaptureResult {
  std::optional<Anchor> anchor;
  std::string diagnostic;
  explicit operator bool() const { return bool(anchor); }
};
CaptureResult captureAnchor(const Vector3d& worldPoint, const Vector3d& worldNormal,
                            const Vector3d& cameraUp, const Transform3d& owner);
}
