// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/SourceInventory.h"
#include "core/providers/ProviderTypes.h"
#include "core/anchors/AnchorOperations.h"
namespace Insertion {
struct PlacementDraft {
  enum class Mode {FixedGeometry,FixedComponent,NamedAttachment} mode=Mode::FixedGeometry;
  Anchors::Anchor fixedTarget;
  std::optional<Anchors::ReferenceKey> namedTarget;
  std::string fixedOwnerPath;
  Vector3d offset=Vector3d::Zero();
  double spinDegrees=0;
  bool flipped=false;
  Anchors::AttachmentMode attachmentMode() const {return flipped?Anchors::AttachmentMode::Align:Anchors::AttachmentMode::Mate;}
  static Result<PlacementDraft> capture(const SourceTarget&,const Providers::GeneratedFragment&,
    const Vector3d& worldPoint,const Vector3d& worldNormal,const Vector3d& cameraUp,
    std::shared_ptr<const Semantic::Scene>,const std::string& pickedOwnerPath={});
  Result<Transform3d> localPlacement(const Providers::GeneratedFragment&) const;
  Result<Transform3d> worldPlacement(const SourceTarget&,const Providers::GeneratedFragment&,
    std::shared_ptr<const Semantic::Scene>) const;
  Result<Transform3d> movementFrame(const SourceTarget&,std::shared_ptr<const Semantic::Scene>) const;
  Result<Vector2d> planeCoordinates(const SourceTarget&,std::shared_ptr<const Semantic::Scene>,
    const Vector3d& rayOrigin,const Vector3d& rayDirection) const;
  static std::optional<double> spinAngle(const Vector2d& coordinates,const Vector2d& origin);
};
}
