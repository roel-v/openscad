#include "gui/clipping/SectionManipulator.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
TEST_CASE("Section ray manipulators have stable signed motion and reject parallel degeneracy","[clipping-math]") {
  auto parameter=Clipping::axisParameter({{2,-10,0},{0,1,0}},Vector3d::Zero(),Vector3d::UnitX());REQUIRE(parameter);REQUIRE(std::abs(*parameter-2)<1e-12);
  REQUIRE_FALSE(Clipping::axisParameter({{0,0,0},{1,0,0}},Vector3d::Zero(),Vector3d::UnitX()));
  auto angle=Clipping::ringAngle({{0,1,5},{0,0,-1}},Vector3d::Zero(),Vector3d::UnitZ(),Vector3d::UnitY());REQUIRE(angle);REQUIRE(std::abs(*angle-std::acos(-1.)/2)<1e-12);
  REQUIRE_FALSE(Clipping::ringAngle({{1,0,0},{1,0,0}},Vector3d::Zero(),Vector3d::UnitZ(),Vector3d::UnitY()));
  auto up=Clipping::transportedUp(Vector3d::UnitZ(),Vector3d::UnitZ());REQUIRE(up.allFinite());REQUIRE(std::abs(up.dot(Vector3d::UnitZ()))<1e-12);
  auto rotation=Clipping::orientation(Vector3d::UnitZ(),Vector3d::UnitY(),Vector3d::UnitX(),up);REQUIRE((rotation*Vector3d::UnitZ()).isApprox(Vector3d::UnitX()));
  REQUIRE(std::abs(Clipping::snappedAngle(.27,true)-std::acos(-1.)/12)<1e-12);
}
