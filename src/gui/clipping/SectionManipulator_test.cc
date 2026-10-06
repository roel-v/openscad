#include "gui/clipping/SectionManipulator.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
TEST_CASE("Section orientations remain stable for parallel up vectors","[clipping-math]") {
  auto up=Clipping::transportedUp(Vector3d::UnitZ(),Vector3d::UnitZ());REQUIRE(up.allFinite());REQUIRE(std::abs(up.dot(Vector3d::UnitZ()))<1e-12);
  auto rotation=Clipping::orientation(Vector3d::UnitZ(),Vector3d::UnitY(),Vector3d::UnitX(),up);REQUIRE((rotation*Vector3d::UnitZ()).isApprox(Vector3d::UnitX()));
}
TEST_CASE("Section orientation transports roll through parallel and antiparallel normals","[clipping-math]") {
  const Vector3d baseNormal=Vector3d::UnitZ(),baseUp=Vector3d::UnitY();
  for(const Vector3d normal:{Vector3d(0,0,1),Vector3d(0,0,-1),Vector3d(0,1,0),Vector3d(1,2,3).normalized().eval()}){
    const auto up=Clipping::transportedUp(normal,baseUp);
    const auto rotation=Clipping::orientation(baseNormal,baseUp,normal,up);
    REQUIRE(rotation.coeffs().allFinite());
    REQUIRE(std::abs(rotation.norm()-1)<1e-12);
    REQUIRE((rotation*baseNormal).isApprox(normal,1e-12));
    REQUIRE((rotation*baseUp).isApprox(up,1e-12));
    REQUIRE(std::abs(up.dot(normal))<1e-12);
    if(std::abs(normal.z())==1)REQUIRE(up.isApprox(baseUp,1e-12));
  }
}
