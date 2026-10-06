#pragma once
#include "glview/CameraPose.h"
#include <QPolygonF>
#include <QString>
#include <vector>

struct NavigationFacet {
  int id;
  Eigen::Vector3d normal;
  std::vector<Eigen::Vector3d> vertices;
  QString label;
};
struct ProjectedNavigationFacet {
  int id;
  QPolygonF polygon;
  QPointF center;
  double facing;
  QString label;
};

class NavigationCubeGeometry {
public:
  static std::vector<NavigationFacet> facets(double chamfer);
  static std::vector<ProjectedNavigationFacet> project(const Eigen::Matrix3d& rotation,
                                                      const QPointF& center, double radius,
                                                      double chamfer);
  static int hit(const std::vector<ProjectedNavigationFacet>& facets, const QPointF& point);
};
