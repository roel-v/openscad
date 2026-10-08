#include "io/DxfData.h"
#include "io/export.h"
#include "geometry/ClipperUtils.h"
#include "geometry/Polygon2d.h"

#include <QTemporaryFile>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <sstream>

TEST_CASE("DXF round trips preserve colored overlaps and same-color holes", "[io][dxf][color]")
{
  Outline2d red;
  red.vertices = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  red.color = Color4f(1.0f, 0.0f, 0.0f, 1.0f);
  Outline2d hole;
  hole.vertices = {{2, 2}, {2, 4}, {4, 4}, {4, 2}};
  hole.positive = false;
  hole.color = red.color;
  Outline2d blue;
  blue.vertices = {{5, 0}, {15, 0}, {15, 10}, {5, 10}};
  blue.color = Color4f(0.0f, 0.0f, 1.0f, 1.0f);
  double expected = 146;
  bool includeBlue = true;

  SECTION("overlapping colors") {}
  SECTION("different colors share endpoints") {
    blue.vertices = {{10, 0}, {20, 0}, {20, 10}, {10, 10}};
    expected = 196;
  }
  SECTION("another color nested inside is not a hole") {
    blue.vertices = {{5, 5}, {7, 5}, {7, 7}, {5, 7}};
    expected = 96;
  }
  SECTION("another color fills a hole") {
    hole.vertices = {{6, 2}, {6, 4}, {8, 4}, {8, 2}};
    expected = 150;
  }
  SECTION("a following uncolored entity does not inherit RGB") {
    blue.color = Color4f{};
  }
  SECTION("uncolored holes retain their existing meaning") {
    red.color = hole.color = Color4f{};
    includeBlue = false;
    expected = 96;
  }

  auto source = std::make_shared<Polygon2d>();
  source->addOutline(red);
  source->addOutline(hole);
  if (includeBlue) source->addOutline(blue);
  std::ostringstream stream;
  export_dxf(source, stream);
  const auto bytes = stream.str();
  QTemporaryFile file;
  REQUIRE(file.open());
  REQUIRE(file.write(bytes.data(), static_cast<qint64>(bytes.size())) ==
          static_cast<qint64>(bytes.size()));
  REQUIRE(file.flush());
  file.close();
  DxfData imported(CurveDiscretizer(36), file.fileName().toUtf8().toStdString());
  const auto polygon = imported.toPolygon2d();
  REQUIRE(polygon->outlines().size() == source->outlines().size());
  for (size_t i = 0; i < polygon->outlines().size(); ++i) {
    REQUIRE(polygon->outlines()[i].color == source->outlines()[i].color);
  }
  // Import evaluation sanitizes each color before the renderer unions the groups.
  const auto sanitized = ClipperUtils::sanitize(*polygon);
  REQUIRE_THAT(std::abs(sanitized->area()), Catch::Matchers::WithinAbs(expected, 1e-8));
}
