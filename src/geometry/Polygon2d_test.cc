#include "geometry/Polygon2d.h"

#include <algorithm>
#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

TEST_CASE("Mixed-color contours retain overlaps and holes when rendered", "[geometry][color]")
{
  Outline2d red;
  red.vertices = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  red.color = Color4f(1, 0, 0, 1);
  Outline2d hole;
  hole.vertices = {{2, 2}, {2, 4}, {4, 4}, {4, 2}};
  hole.positive = false;
  hole.color = red.color;
  Outline2d blue;
  blue.vertices = {{5, 0}, {15, 0}, {15, 10}, {5, 10}};
  blue.color = Color4f(0, 0, 1, 1);

  SECTION("ordinary contours") {}
  SECTION("one color has mirrored winding") {
    std::reverse(blue.vertices.begin(), blue.vertices.end());
  }
  SECTION("all contours have mirrored winding") {
    std::reverse(red.vertices.begin(), red.vertices.end());
    std::reverse(hole.vertices.begin(), hole.vertices.end());
    std::reverse(blue.vertices.begin(), blue.vertices.end());
  }

  Polygon2d polygon;
  polygon.addOutline(red);
  polygon.addOutline(hole);
  polygon.addOutline(blue);
  const auto merged = polygon.unionedContours();
  REQUIRE(merged);
  REQUIRE(merged->outlines().size() == 2);
  REQUIRE_THAT(std::abs(merged->area()), Catch::Matchers::WithinAbs(146.0, 1e-8));
  REQUIRE_THAT(std::abs(polygon.area()), Catch::Matchers::WithinAbs(146.0, 1e-8));
  // Export still needs the original separately colored outlines.
  REQUIRE(polygon.outlines().size() == 3);
  REQUIRE(polygon.outlines()[0].color == red.color);
  REQUIRE(polygon.outlines()[2].color == blue.color);
}
