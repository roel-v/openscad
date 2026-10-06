#include "gui/NavigationCubeGeometry.h"
#include <algorithm>
#include <cmath>

std::vector<NavigationFacet> NavigationCubeGeometry::facets(double chamfer)
{
  const double a = 1 - std::clamp(chamfer, 0.08, 0.45);
  std::vector<NavigationFacet> result;
  for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
    const Eigen::Vector3d signs(x,y,z);
    const int count = std::abs(x) + std::abs(y) + std::abs(z);
    if (!count) continue;
    NavigationFacet face{static_cast<int>(result.size()), signs.normalized(), {}, {}};
    std::vector<int> axes;
    for (int axis = 0; axis < 3; ++axis) if (signs[axis]) axes.push_back(axis);
    if (count == 1) {
      const int axis = axes[0], u = (axis + 1) % 3, v = (axis + 2) % 3;
      for (int su : {-1,1}) for (int sv : {-1,1}) {
        Eigen::Vector3d p = signs;
        p[u] = su*a; p[v] = sv*a;
        face.vertices.push_back(p);
      }
      static const char *negative[] = {"LEFT", "FRONT", "BOTTOM"};
      static const char *positive[] = {"RIGHT", "BACK", "TOP"};
      face.label = QString::fromLatin1(signs[axis] < 0 ? negative[axis] : positive[axis]);
    } else if (count == 2) {
      const int freeAxis = 3 - axes[0] - axes[1];
      for (int axis : axes) for (int side : {-1,1}) {
        Eigen::Vector3d p = a*signs;
        p[axis] = signs[axis]; p[freeAxis] = side*a;
        face.vertices.push_back(p);
      }
    } else {
      for (int axis : axes) {
        Eigen::Vector3d p = a*signs;
        p[axis] = signs[axis];
        face.vertices.push_back(p);
      }
    }
    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    for (const auto& p : face.vertices) center += p;
    center /= static_cast<double>(face.vertices.size());
    const Eigen::Vector3d u = (face.vertices[0]-center).normalized();
    const Eigen::Vector3d v = face.normal.cross(u);
    std::sort(face.vertices.begin(), face.vertices.end(), [&](const auto& p, const auto& q) {
      return std::atan2((p-center).dot(v), (p-center).dot(u)) <
             std::atan2((q-center).dot(v), (q-center).dot(u));
    });
    result.push_back(std::move(face));
  }
  return result;
}

std::vector<ProjectedNavigationFacet> NavigationCubeGeometry::project(
  const Eigen::Matrix3d& rotation, const QPointF& center, double radius, double chamfer)
{
  std::vector<ProjectedNavigationFacet> result;
  for (const auto& face : facets(chamfer)) {
    const double facing = -(rotation*face.normal).y();
    if (facing <= 1e-7) continue;
    ProjectedNavigationFacet projected{face.id, {}, {}, facing, face.label};
    for (const auto& point : face.vertices) {
      const Eigen::Vector3d p = rotation*point;
      const QPointF pixel = center + radius*QPointF(p.x(), -p.z());
      projected.polygon << pixel;
      projected.center += pixel;
    }
    projected.center /= static_cast<double>(face.vertices.size());
    result.push_back(std::move(projected));
  }
  return result;
}

int NavigationCubeGeometry::hit(const std::vector<ProjectedNavigationFacet>& facets, const QPointF& point)
{
  // A convex solid has no overlapping visible interiors. Stable ID order breaks shared-edge ties.
  for (const auto& face : facets) if (face.polygon.containsPoint(point, Qt::OddEvenFill)) return face.id;
  return -1;
}
