#pragma once
#include "geometry/linalg.h"
#include <optional>
#include <array>
#include <cstdint>

namespace SurfacePickMath {
constexpr uint32_t MaxDrawId = 0xffffff;
std::optional<Eigen::Vector2i> pixel(double x, double y, double dpr, int width, int height);
std::optional<Vector3d> unproject(int x, int y, double depth, int width, int height,
  const Eigen::Matrix4d& projection, const Eigen::Matrix4d& modelview, const Vector3d& positionOrigin);
std::array<uint8_t, 3> encode(uint32_t id);
uint32_t decode(const std::array<uint8_t, 3>& rgb);
}
