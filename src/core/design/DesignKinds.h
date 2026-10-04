#pragma once
#include "core/design/DesignObjects.h"
#include <Eigen/Geometry>

namespace Design {
struct Section : Payload {
  Vector3d origin = Vector3d::Zero(), normal = Vector3d::UnitZ(), up = Vector3d::UnitY();
  Vector3d translation = Vector3d::Zero();
  Eigen::Quaterniond rotation = Eigen::Quaterniond::Identity();
  double offset = 0;
  ReferenceToken target, frame;
};
struct View : Payload {
  bool perspective = false, fixed = false;
  Vector3d direction = Vector3d::UnitY(), up = Vector3d::UnitZ();
  double margin = 0.05, distance = 0, fov = 45;
  Vector3d cameraRotation = Vector3d::Zero(), cameraTranslation = Vector3d::Zero();
  ReferenceToken target, frame;
};
bool registerKind(KindHandler handler);
std::shared_ptr<const KindHandler> kind(const std::string& name);
// Startup registration is explicit and idempotent; duplicate extension registration fails.
void registerBuiltins();
DecodeResult decodeSection(const Value&, const DeclarationContext&);
DecodeResult decodeView(const Value&, const DeclarationContext&);
std::optional<ReferenceToken> referenceToken(const Value&, const std::string& defaultDomain);
}
