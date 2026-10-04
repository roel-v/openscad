#pragma once
#include "geometry/linalg.h"
#include <cstdint>
#include <string>
#include <vector>

struct ClipPlane {
  Vector3d origin = Vector3d::Zero();
  Vector3d normal = Vector3d::UnitZ();
  double epsilon = 0;
  std::string sectionId;
};
struct ClipState {
  static constexpr unsigned MaxPlanes = 8;
  uint64_t generation = 0;
  std::vector<ClipPlane> planes;
  bool retains(const Vector3d& worldPoint) const;
};
struct ClipCapabilities {
  bool available = false, caps = false;
  unsigned maxPlanes = ClipState::MaxPlanes;
  std::string reason;
};
double clippingTolerance(const BoundingBox& bounds, const Vector3d& planeOrigin);
ClipPlane prepareClipPlane(const Vector3d& origin, const Vector3d& normal,
                           const BoundingBox& bounds, std::string sectionId = {});
ClipState prepareClipState(std::vector<ClipPlane> planes, const BoundingBox& bounds,
                          uint64_t generation);
