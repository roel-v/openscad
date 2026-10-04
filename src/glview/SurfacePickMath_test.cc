#include "glview/SurfacePickMath.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <stdexcept>

TEST_CASE("Surface pixels use physical pixel centers and all viewport borders", "[surface-pick-math]")
{
  for (double dpr : {1.0, 1.5, 2.0}) {
    const int width = static_cast<int>(200*dpr), height = static_cast<int>(100*dpr);
    REQUIRE(*SurfacePickMath::pixel(0,0,dpr,width,height) == Eigen::Vector2i(0,height-1));
    REQUIRE(*SurfacePickMath::pixel(200-0.5/dpr,0,dpr,width,height) == Eigen::Vector2i(width-1,height-1));
    REQUIRE(*SurfacePickMath::pixel(0,100-0.5/dpr,dpr,width,height) == Eigen::Vector2i(0,0));
    REQUIRE(*SurfacePickMath::pixel(200-0.5/dpr,100-0.5/dpr,dpr,width,height) == Eigen::Vector2i(width-1,0));
    REQUIRE_FALSE(SurfacePickMath::pixel(200,50,dpr,width,height));
    REQUIRE_FALSE(SurfacePickMath::pixel(20,100,dpr,width,height));
    REQUIRE_FALSE(SurfacePickMath::pixel(-0.01,0,dpr,width,height));
  }
  REQUIRE_FALSE(SurfacePickMath::pixel(1,1,0,20,20));
  REQUIRE_FALSE(SurfacePickMath::pixel(NAN,1,1,20,20));
  REQUIRE_FALSE(SurfacePickMath::pixel(1,1,1,0,20));
}

TEST_CASE("Surface unprojection round trips perspective orthographic and recentered coordinates", "[surface-pick-math]")
{
  Eigen::Matrix4d perspective = Eigen::Matrix4d::Zero();
  perspective(0,0)=1.7; perspective(1,1)=2.1;
  perspective(2,2)=-1.02; perspective(2,3)=-0.202; perspective(3,2)=-1;
  Eigen::Matrix4d orthographic = Eigen::Matrix4d::Identity();
  orthographic(0,0)=0.1; orthographic(1,1)=0.2; orthographic(2,2)=-0.01;
  for (const Eigen::Matrix4d& projection : {perspective, orthographic}) {
    for (Vector3d origin : {Vector3d::Zero().eval(), Vector3d(10000,-20000,30000)}) {
      Eigen::Matrix4d modelview = Eigen::Matrix4d::Identity();
      modelview(2,3)=-10;
      const int x=83, y=37, width=200, height=100;
      const auto point = SurfacePickMath::unproject(x,y,0.4,width,height,projection,modelview,origin);
      REQUIRE(point);
      const Vector3d local = *point-origin;
      Eigen::Vector4d projected = projection*modelview*Eigen::Vector4d(local.x(),local.y(),local.z(),1);
      projected /= projected.w();
      REQUIRE(std::abs((projected.x()+1)*width/2-(x+0.5)) < 1e-7);
      REQUIRE(std::abs((projected.y()+1)*height/2-(y+0.5)) < 1e-7);
      REQUIRE(std::abs((projected.z()+1)/2-0.4) < 1e-7);
    }
  }
  const Eigen::Matrix4d identity = Eigen::Matrix4d::Identity();
  REQUIRE_FALSE(SurfacePickMath::unproject(1,1,1,20,20,identity,identity,Vector3d::Zero()));
  REQUIRE_FALSE(SurfacePickMath::unproject(1,1,NAN,20,20,identity,identity,Vector3d::Zero()));
  REQUIRE_FALSE(SurfacePickMath::unproject(1,1,0.5,20,20,identity,Eigen::Matrix4d::Zero(),Vector3d::Zero()));
}

TEST_CASE("Surface draw IDs reserve zero and reject 24 bit overflow", "[surface-pick-math]")
{
  for (const uint32_t id : {0u,1u,255u,256u,65536u,SurfacePickMath::MaxDrawId})
    REQUIRE(SurfacePickMath::decode(SurfacePickMath::encode(id)) == id);
  REQUIRE_THROWS_AS(SurfacePickMath::encode(SurfacePickMath::MaxDrawId+1), std::overflow_error);
}
