#include "gui/NavigationCubeGeometry.h"
#include <catch2/catch_test_macros.hpp>
#include <set>

TEST_CASE("Chamfered cube has 26 stable planar outward facets", "[navigation]")
{
  const auto faces = NavigationCubeGeometry::facets(0.22);
  REQUIRE(faces.size()==26);
  int squares=0, corners=0, edges=0;
  for (const auto& face : faces) {
    if (!face.label.isEmpty()) ++squares;
    else if (face.vertices.size()==3) ++corners;
    else ++edges;
    for (const auto& p : face.vertices) {
      REQUIRE(std::abs(p.dot(face.normal)-face.vertices[0].dot(face.normal)) < 1e-12);
    }
    const auto rotation = CameraPose::matrix(CameraPose::upright(face.normal));
    const auto projected = NavigationCubeGeometry::project(rotation,{0,0},100,0.22);
    REQUIRE(NavigationCubeGeometry::hit(projected,{0,0}) == face.id);
    for (const auto& visible : projected) REQUIRE(visible.facing > 0);
  }
  REQUIRE(squares==6);
  REQUIRE(edges==12);
  REQUIRE(corners==8);
}

TEST_CASE("Isometric targets have equal projected axis lengths", "[navigation]")
{
  const auto matrix = CameraPose::matrix(CameraPose::upright({1,-1,1}));
  const auto screenLength = [&](int axis) {
    const Eigen::Vector3d p=matrix.col(axis);
    return std::hypot(p.x(),p.z());
  };
  REQUIRE(std::abs(screenLength(0)-screenLength(1)) < 1e-12);
  REQUIRE(std::abs(screenLength(1)-screenLength(2)) < 1e-12);
  const auto projected = NavigationCubeGeometry::project(matrix,{100,100},40,0.22);
  REQUIRE(NavigationCubeGeometry::hit(projected,{0,0}) == -1);
  for (const auto& facet : projected) REQUIRE(NavigationCubeGeometry::hit(projected,facet.center)==facet.id);
}
