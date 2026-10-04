#include "glview/ClippingState.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

double clippingTolerance(const BoundingBox& bounds, const Vector3d& origin)
{
  if (bounds.isEmpty() || !bounds.min().allFinite() || !bounds.max().allFinite() || !origin.allFinite())
    throw std::invalid_argument("Section bounds and origin must be finite and available");
  const double diagonal = bounds.sizes().stableNorm();
  if (!(diagonal > 0) || !std::isfinite(diagonal))
    throw std::invalid_argument("Section display requires a positive finite bounds diagonal");
  const double magnitude = std::max({diagonal, bounds.center().stableNorm(), origin.stableNorm()});
  const double epsilon = std::max(8 * std::numeric_limits<float>::epsilon() * diagonal,
                                64 * std::numeric_limits<double>::epsilon() * magnitude);
  if (!std::isfinite(epsilon)) throw std::invalid_argument("Section coordinates exceed supported precision");
  return epsilon;
}
ClipPlane prepareClipPlane(const Vector3d& origin, const Vector3d& normal,
                           const BoundingBox& bounds, std::string id)
{
  const double length = normal.stableNorm();
  if (!normal.allFinite() || !std::isfinite(length) || !(length > 0))
    throw std::invalid_argument("Section normal must be finite and nonzero");
  return {origin, normal / length, clippingTolerance(bounds, origin), std::move(id)};
}
ClipState prepareClipState(std::vector<ClipPlane> planes, const BoundingBox& bounds,
                          uint64_t generation)
{
  if (planes.size() > ClipState::MaxPlanes)
    throw std::invalid_argument("At most eight Sections can be enabled simultaneously");
  for (auto& plane : planes)
    plane = prepareClipPlane(plane.origin, plane.normal, bounds, std::move(plane.sectionId));
  return {generation, std::move(planes)};
}
bool ClipState::retains(const Vector3d& point) const
{
  if (!point.allFinite()) return false;
  return std::all_of(planes.begin(), planes.end(), [&](const ClipPlane& plane) {
    return (point - plane.origin).dot(plane.normal) > plane.epsilon;
  });
}
