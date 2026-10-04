#pragma once
#include "geometry/linalg.h"
#include <Eigen/Geometry>
#include <optional>
namespace Clipping {
struct Ray { Vector3d origin, direction; };
std::optional<double> axisParameter(const Ray&, const Vector3d& origin, const Vector3d& axis);
std::optional<double> ringAngle(const Ray&, const Vector3d& origin, const Vector3d& normal, const Vector3d& up);
Vector3d transportedUp(const Vector3d& normal, const Vector3d& previousUp);
Eigen::Quaterniond orientation(const Vector3d& baseNormal, const Vector3d& baseUp,
                              const Vector3d& normal, const Vector3d& up);
double snappedAngle(double radians, bool snap);
}
