#include "glview/SurfacePickMath.h"
#include <Eigen/LU>
#include <cmath>
#include <stdexcept>

namespace SurfacePickMath {
std::optional<Eigen::Vector2i> pixel(double x, double y, double dpr, int width, int height)
{
  if (width <= 0 || height <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(dpr) || dpr <= 0 || x < 0 || y < 0) return {};
  const double px = std::floor(x * dpr), py = std::floor(y * dpr);
  if (px >= width || py >= height || !std::isfinite(px) || !std::isfinite(py)) return {};
  return Eigen::Vector2i(static_cast<int>(px), height - 1 - static_cast<int>(py));
}
std::optional<Vector3d> unproject(int x, int y, double depth, int width, int height,
  const Eigen::Matrix4d& projection, const Eigen::Matrix4d& modelview, const Vector3d& origin)
{
  if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height ||
      !std::isfinite(depth) || depth < 0 || depth >= 1 || !origin.allFinite()) return {};
  const Eigen::Matrix4d combined = projection * modelview;
  if (!combined.allFinite()) return {};
  Eigen::FullPivLU<Eigen::Matrix4d> inverse(combined);
  if (!inverse.isInvertible()) return {};
  Eigen::Vector4d point = inverse.solve(Eigen::Vector4d(
    2 * (x + 0.5) / width - 1, 2 * (y + 0.5) / height - 1, 2 * depth - 1, 1));
  if (!point.allFinite() || std::abs(point.w()) < 1e-15) return {};
  const Vector3d world = point.head<3>() / point.w() + origin;
  return world.allFinite() ? std::optional<Vector3d>(world) : std::nullopt;
}
std::array<uint8_t, 3> encode(uint32_t id)
{
  if (id > MaxDrawId) throw std::overflow_error("Surface draw ID overflow");
  return {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id >> 16)};
}
uint32_t decode(const std::array<uint8_t, 3>& rgb)
{
  return uint32_t(rgb[0]) | (uint32_t(rgb[1]) << 8) | (uint32_t(rgb[2]) << 16);
}
}
