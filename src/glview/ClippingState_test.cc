#include "glview/ClippingState.h"
#include <stdexcept>
#include <catch2/catch_test_macros.hpp>
#include <cfloat>

TEST_CASE("Clipping retains the strict positive intersection and refuses plane nine", "[clipping-math]")
{
  const BoundingBox bounds(Vector3d(-1,-1,-1),Vector3d(1,1,1));
  auto state=prepareClipState({{{0,0,0},{1,0,0}},{{0,0,0},{0,1,0}}},bounds,9);
  REQUIRE(state.generation==9);REQUIRE(state.retains({1,1,0}));
  REQUIRE_FALSE(state.retains({0,1,0}));REQUIRE_FALSE(state.retains({1,-1,0}));
  REQUIRE(prepareClipState({},BoundingBox(),10).retains({-10,-10,-10}));
  auto nine=std::vector<ClipPlane>(9);REQUIRE_THROWS_AS(prepareClipState(nine,bounds,11),std::invalid_argument);
  REQUIRE(state.planes.size()==2);
}
TEST_CASE("Clipping tolerance removes coincident faces and retains separate thin walls across scales", "[clipping-math]")
{
  for(double scale:{1e-4,1.,1e4})for(double translation:{0.,1e5}){
    Vector3d center=scale*Vector3d(translation,-translation,translation);
    BoundingBox bounds(center-scale*Vector3d::Ones(),center+scale*Vector3d::Ones());
    for(Vector3d normal:{Vector3d::UnitZ().eval(),Vector3d(1,2,-3).normalized().eval(),Vector3d(-1,2,3).normalized().eval()}){
      auto state=prepareClipState({{center,normal}},bounds,1);double diagonal=bounds.sizes().norm();
      REQUIRE_FALSE(state.retains(center));REQUIRE_FALSE(state.retains(center-normal*diagonal*1e-4));
      REQUIRE(state.retains(center+normal*diagonal*1e-4));REQUIRE(state.planes[0].epsilon<diagonal*1e-4);
      REQUIRE(state.planes[0].epsilon>=8*FLT_EPSILON*diagonal);
    }
  }
}
TEST_CASE("Clipping rejects nonfinite degenerate inputs and normalizes without changing the plane", "[clipping-math]")
{
  const BoundingBox bounds(Vector3d::Zero(),Vector3d::Ones());
  auto plane=prepareClipPlane({0,0,.5},{0,0,2},bounds,"stable");REQUIRE(plane.normal.isApprox(Vector3d::UnitZ()));REQUIRE(plane.sectionId=="stable");
  REQUIRE_THROWS_AS(prepareClipPlane({0,0,0},Vector3d::Zero(),bounds),std::invalid_argument);
  REQUIRE_THROWS_AS(prepareClipPlane({NAN,0,0},{0,0,1},bounds),std::invalid_argument);
  REQUIRE_THROWS_AS(prepareClipPlane({0,0,0},{INFINITY,0,1},bounds),std::invalid_argument);
  REQUIRE_THROWS_AS(clippingTolerance(BoundingBox(),Vector3d::Zero()),std::invalid_argument);
  REQUIRE_FALSE(ClipState{}.retains({NAN,0,0}));
}
