#include "glview/CameraPose.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Navigation camera basis matches native principal views", "[navigation]")
{
  const Eigen::Vector3d normals[] = {{0,-1,0}, {0,1,0}, {-1,0,0}, {1,0,0}, {0,0,1}, {0,0,-1}};
  const Eigen::Vector3d native[] = {{0,0,0}, {0,0,180}, {0,0,90}, {0,0,270}, {90,0,0}, {270,0,0}};
  for (int i = 0; i < 6; ++i) {
    const auto matrix = CameraPose::matrix(CameraPose::upright(normals[i]));
    REQUIRE(matrix.isApprox(CameraPose::matrix(native[i]), 1e-12));
    REQUIRE((matrix * normals[i]).isApprox(Eigen::Vector3d(0,-1,0), 1e-12));
  }
  for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
      for (int z = -1; z <= 1; ++z) {
        if (!x && !y && !z) continue;
        Eigen::Vector3d n(x,y,z);
        const auto matrix = CameraPose::matrix(CameraPose::upright(n));
        REQUIRE((matrix * n.normalized()).isApprox(Eigen::Vector3d(0,-1,0), 1e-12));
        REQUIRE(matrix.determinant() > 0.999999999);
      }
}

TEST_CASE("Camera snapshots preserve execution state and Euler round trips", "[navigation]")
{
  Camera camera;
  camera.locked = true;
  camera.viewall = true;
  camera.autocenter = true;
  camera.pixel_width = 123;
  camera.pixel_height = 456;
  const auto pose = CameraPose::from(camera);
  auto changed = pose;
  changed.rotation = {91, 37, 359};
  changed.translation = {5, -7, 11};
  changed.projection = Camera::ProjectionType::ORTHOGONAL;
  changed.apply(camera);
  REQUIRE(CameraPose::from(camera) == changed);
  REQUIRE(camera.locked);
  REQUIRE(camera.viewall);
  REQUIRE(camera.autocenter);
  REQUIRE(camera.pixel_width == 123);
  REQUIRE(camera.pixel_height == 456);
  for (const Eigen::Vector3d a : {Eigen::Vector3d(0,90,33), Eigen::Vector3d(-450,27,721),
                                 Eigen::Vector3d(89.999,-90,120)}) {
    const auto m = CameraPose::matrix(a);
    REQUIRE(CameraPose::matrix(CameraPose::angles(m)).isApprox(m, 1e-12));
  }
}
