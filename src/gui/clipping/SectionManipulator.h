#pragma once
#include "geometry/linalg.h"
#include <Eigen/Geometry>
namespace Clipping {
Vector3d transportedUp(const Vector3d& normal, const Vector3d& previousUp);
Eigen::Quaterniond orientation(const Vector3d& baseNormal, const Vector3d& baseUp,
                              const Vector3d& normal, const Vector3d& up);
}
