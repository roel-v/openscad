#include "geometry/analysis/GeometryAnalysis.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "geometry/Geometry.h"
#include "geometry/PolySet.h"
#include "geometry/Polygon2d.h"
#ifdef ENABLE_MANIFOLD
#include "geometry/manifold/ManifoldGeometry.h"
#endif
#ifdef ENABLE_CGAL
#include "geometry/cgal/CGALUtils.h"
#include "geometry/cgal/CGALNefGeometry.h"
#endif

using namespace GeometryAnalysis;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
std::shared_ptr<PolySet> box()
{
  auto poly = std::make_shared<PolySet>(3);
  poly->vertices = {{0, 0, 0}, {20, 0, 0}, {20, 10, 0}, {0, 10, 0},
                    {0, 0, 6}, {20, 0, 6}, {20, 10, 6}, {0, 10, 6}};
  poly->indices = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                   {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                   {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
  poly->setTriangular(true);
  return poly;
}
Plane middle() { return {{0, 0, 3}, {0, 0, 1}, {0, 1, 0}}; }
void close(double actual, double expected)
{
  REQUIRE_THAT(actual, WithinAbs(expected, 1e-7) || WithinRel(expected, 1e-8));
}
#ifdef ENABLE_MANIFOLD
std::shared_ptr<ManifoldGeometry> evaluatedBox()
{
  return std::make_shared<ManifoldGeometry>(manifold::Manifold::Cube({20, 10, 6}));
}
void checkSection(const SectionResult& result, double area, size_t outer, size_t holes = 0)
{
  INFO(result.diagnostic.detail);
  REQUIRE(result.diagnostic.error == Error::None);
  REQUIRE(result.area);
  REQUIRE(result.sectionToWorld);
  close(*result.area, area);
  size_t actualOuter = 0, actualHoles = 0;
  for (const auto& contour : result.contours) {
    if (contour.hole) ++actualHoles; else ++actualOuter;
    REQUIRE(contour.points.size() >= 3);
    REQUIRE(contour.points.front() != contour.points.back());
    for (const auto& point : contour.points) {
      REQUIRE(point.allFinite());
      REQUIRE_FALSE(point.x() < contour.points.front().x());
    }
    double twiceArea = 0;
    for (size_t j = 0; j < contour.points.size(); ++j) {
      const auto& a = contour.points[j];
      const auto& b = contour.points[(j + 1) % contour.points.size()];
      twiceArea += a.x() * b.y() - b.x() * a.y();
    }
    REQUIRE((contour.hole ? twiceArea < 0 : twiceArea > 0));
  }
  REQUIRE(actualOuter == outer);
  REQUIRE(actualHoles == holes);
}
#endif
}

TEST_CASE("Geometry analysis bounds use referenced transformed vertices", "[geometry-analysis]")
{
  auto mesh = box();
  mesh->vertices.emplace_back(10000, 10000, 10000);
  Input input{mesh};
  const auto before = mesh->vertices;
  auto result = bounds(input);
  REQUIRE(result.diagnostic.error == Error::None);
  REQUIRE(result.bounds);
  REQUIRE(result.bounds->min() == Vector3d(0, 0, 0));
  REQUIRE(result.bounds->max() == Vector3d(20, 10, 6));
  REQUIRE(mesh->vertices == before);
  REQUIRE(input.objectToWorld.matrix() == Transform3d::Identity().matrix());

  mesh->indices.pop_back(); // Bounds also work for open meshes.
  REQUIRE(bounds(input).diagnostic.error == Error::None);
  auto triangle = std::make_shared<PolySet>(3);
  triangle->vertices = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
  triangle->indices = {{0, 1, 2}};
  input.geometry = triangle;
  input.objectToWorld.rotate(Eigen::AngleAxisd(std::acos(-1.0) / 4, Vector3d::UnitZ()));
  result = bounds(input);
  REQUIRE(result.bounds);
  REQUIRE_THAT(result.bounds->max().y(), WithinAbs(std::sqrt(2.0), 1e-7));
}

TEST_CASE("Geometry analysis rejects invalid inputs without payload", "[geometry-analysis]")
{
  Input input;
  REQUIRE(bounds(input).diagnostic.error == Error::InvalidInput);
  input.geometry = std::make_shared<Polygon2d>();
  REQUIRE(bounds(input).diagnostic.error == Error::UnsupportedGeometry);
  input.geometry = std::make_shared<GeometryList>(Geometry::Geometries{});
  REQUIRE(bounds(input).diagnostic.error == Error::UnsupportedGeometry);
  input.geometry = PolySet::createEmpty();
  REQUIRE(bounds(input).diagnostic.error == Error::None);
  REQUIRE_FALSE(bounds(input).bounds);
  auto mesh = box();
  input.geometry = mesh;
  mesh->indices[0][0] = mesh->vertices.size();
  REQUIRE(bounds(input).diagnostic.error == Error::InvalidTopology);
  REQUIRE_FALSE(bounds(input).bounds);
  mesh->indices[0][0] = -1;
  REQUIRE(bounds(input).diagnostic.error == Error::InvalidTopology);
  mesh->indices[0][0] = 0;
  mesh->vertices[0].x() = std::numeric_limits<double>::infinity();
  REQUIRE(bounds(input).diagnostic.error == Error::InvalidInput);
  input.geometry = box();
  input.objectToWorld.linear()(0, 0) = 0;
  REQUIRE(bounds(input).diagnostic.error == Error::InvalidInput);
  input.objectToWorld = Transform3d::Identity();
  input.objectToWorld.matrix()(3, 0) = 1;
  REQUIRE(bounds(input).diagnostic.error == Error::InvalidInput);
  input.objectToWorld = Transform3d::Identity();
  input.objectToWorld.matrix()(0, 0) = std::numeric_limits<double>::quiet_NaN();
  REQUIRE(bounds(input).diagnostic.error == Error::InvalidInput);
}

TEST_CASE("Geometry analysis honors limits and cancellation", "[geometry-analysis]")
{
  Input input{box()};
  input.limits.maxVertices = 7;
  REQUIRE(bounds(input).diagnostic.error == Error::LimitExceeded);
  input.limits.maxVertices = 0;
  input.limits.maxTriangles = 11;
  REQUIRE(bounds(input).diagnostic.error == Error::LimitExceeded);
  input.limits.maxTriangles = 0;
  input.limits.cancelled = [] { return true; };
  REQUIRE(bounds(input).diagnostic.error == Error::Cancelled);
  int checks = 0;
  input.limits.cancelled = [&] { return ++checks > 12; };
  const auto result = bounds(input);
  REQUIRE(result.diagnostic.error == Error::Cancelled);
  REQUIRE_FALSE(result.bounds);
}

TEST_CASE("Geometry analysis validates arbitrary section directions", "[geometry-analysis]")
{
  Input input{box()};
  auto plane = middle();
  plane.normal.setZero();
  REQUIRE(section(input, plane).diagnostic.error == Error::InvalidInput);
  plane = middle();
  plane.up = plane.normal;
  REQUIRE(section(input, plane).diagnostic.error == Error::InvalidInput);
  plane.up = Vector3d(0, 1e-10, 1);
  REQUIRE(section(input, plane).diagnostic.error == Error::InvalidInput);
  plane = middle();
  plane.origin.x() = std::numeric_limits<double>::quiet_NaN();
  REQUIRE(section(input, plane).diagnostic.error == Error::InvalidInput);
}

#ifdef ENABLE_MANIFOLD
TEST_CASE("Geometry analysis strict conversion rejects unrepaired topology", "[geometry-analysis]")
{
  auto mesh = box();
  REQUIRE_FALSE(mesh->isManifold()); // Unknown is checked, not treated as invalid.
  REQUIRE(measureSolid(Input{mesh}).diagnostic.error == Error::None);
  mesh->indices.pop_back();
  REQUIRE(measureSolid(Input{mesh}).diagnostic.error == Error::InvalidTopology);
  auto soup = std::make_shared<PolySet>(3);
  const auto indexed = box();
  for (const auto& face : indexed->indices) {
    const int start = static_cast<int>(soup->vertices.size());
    for (const auto index : face) soup->vertices.push_back(indexed->vertices[index]);
    soup->indices.push_back({start, start + 1, start + 2});
  }
  soup->setTriangular(true);
  const auto before = soup->vertices;
  const auto result = measureSolid(Input{soup});
  REQUIRE(result.diagnostic.error == Error::InvalidTopology);
  REQUIRE_FALSE(result.volume);
  REQUIRE_FALSE(result.surfaceArea);
  REQUIRE(soup->vertices == before);
  auto quads = box();
  quads->setTriangular(false);
  quads->indices = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                    {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
  REQUIRE(measureSolid(Input{quads}).diagnostic.error == Error::None);
  for (auto& vertex : quads->vertices) vertex.x() += 1e9;
  const auto limited = measureSolid(Input{quads});
  REQUIRE(limited.diagnostic.error == Error::BackendFailure);
  REQUIRE_FALSE(limited.volume);
}

TEST_CASE("Geometry analysis measures indexed and evaluated solids", "[geometry-analysis]")
{
  for (const auto& geometry : std::vector<std::shared_ptr<const Geometry>>{box(), evaluatedBox()}) {
    const auto result = measureSolid(Input{geometry});
    INFO(result.diagnostic.detail);
    REQUIRE(result.diagnostic.error == Error::None);
    REQUIRE(result.volume);
    REQUIRE(result.surfaceArea);
    REQUIRE(result.diagnostic.backendTolerance);
    REQUIRE(*result.diagnostic.backendTolerance >= 0);
    close(*result.volume, 1200);
    close(*result.surfaceArea, 760);
    checkSection(section(Input{geometry}, middle()), 200, 1);
  }
  const auto mesh = box();
  const auto before = mesh->vertices;
  Input scaled{mesh};
  scaled.objectToWorld.scale(Vector3d(2, 3, 4));
  const auto measured = measureSolid(scaled);
  REQUIRE(measured.volume);
  REQUIRE(measured.surfaceArea);
  close(*measured.volume, 28800);
  close(*measured.surfaceArea, 5760);
  const auto bounded = bounds(scaled);
  REQUIRE(bounded.bounds);
  REQUIRE(bounded.bounds->sizes() == Vector3d(40, 30, 24));
  scaled.objectToWorld = Transform3d::Identity();
  scaled.objectToWorld.scale(Vector3d(-1, 1, 1));
  const auto mirrored = measureSolid(scaled);
  REQUIRE(mirrored.volume);
  close(*mirrored.volume, 1200);
  checkSection(section(scaled, middle()), 200, 1);
  REQUIRE(mesh->vertices == before);
  const auto empty = measureSolid(Input{PolySet::createEmpty()});
  INFO(empty.diagnostic.detail);
  REQUIRE(empty.diagnostic.error == Error::None);
  REQUIRE(empty.volume == 0);
  REQUIRE(empty.surfaceArea == 0);
  REQUIRE_FALSE(empty.diagnostic.backendTolerance);
  checkSection(section(Input{PolySet::createEmpty()}, middle()), 0, 0);
}

TEST_CASE("Geometry analysis section frames are right handed and transform covariant", "[geometry-analysis]")
{
  Input input{box()};
  const auto original = section(input, middle());
  checkSection(original, 200, 1);
  REQUIRE(original.sectionToWorld->linear() == Matrix3d::Identity());
  REQUIRE(original.sectionToWorld->translation() == Vector3d(0, 0, 3));
  const Vector3d local(2, 4, 0);
  REQUIRE((original.sectionToWorld->inverse() * (*original.sectionToWorld * local)).isApprox(local));
  input.objectToWorld.translate(Vector3d(7, -4, 2));
  input.objectToWorld.rotate(Eigen::AngleAxisd(0.7, Vector3d(1, 2, 3).normalized()));
  Plane rotated{input.objectToWorld * middle().origin,
    input.objectToWorld.linear() * middle().normal, input.objectToWorld.linear() * middle().up};
  const auto transformed = section(input, rotated);
  checkSection(transformed, 200, 1);
  REQUIRE(transformed.sectionToWorld->linear().col(0).cross(transformed.sectionToWorld->linear().col(1))
    .isApprox(transformed.sectionToWorld->linear().col(2), 1e-8));
  REQUIRE(transformed.contours[0].points.size() == original.contours[0].points.size());
  for (size_t j = 0; j < original.contours[0].points.size(); ++j) {
    REQUIRE((transformed.contours[0].points[j] - original.contours[0].points[j]).norm() < 1e-7);
  }
  checkSection(section(Input{box()}, {{10, 0, 3}, {1, 0, 1}, {0, 1, 0}}), 60 * std::sqrt(2.0), 1);
}

TEST_CASE("Geometry analysis sections preserve holes and disconnected contours", "[geometry-analysis]")
{
  const auto outer = manifold::Manifold::Cube({20, 10, 6});
  const auto cutter = manifold::Manifold::Cube({10, 4, 8}).Translate({5, 3, -1});
  Input input{std::make_shared<ManifoldGeometry>(outer - cutter)};
  const auto measured = measureSolid(input);
  REQUIRE(measured.volume);
  close(*measured.volume, 960);
  const auto cut = section(input, middle());
  checkSection(cut, 160, 1, 1);
  REQUIRE_FALSE(cut.contours[0].hole);
  REQUIRE(cut.contours[1].hole);
  const auto repeated = section(input, middle());
  REQUIRE(repeated.contours.size() == cut.contours.size());
  for (size_t j = 0; j < cut.contours.size(); ++j) {
    REQUIRE(repeated.contours[j].hole == cut.contours[j].hole);
    REQUIRE(repeated.contours[j].points == cut.contours[j].points);
  }
  input.geometry = std::make_shared<ManifoldGeometry>(outer + outer.Translate({30, 0, 0}));
  const auto disjoint = measureSolid(input);
  REQUIRE(disjoint.volume);
  close(*disjoint.volume, 2400);
  checkSection(section(input, middle()), 400, 2);
}

TEST_CASE("Geometry analysis slices use the half open boundary convention", "[geometry-analysis]")
{
  Input input{evaluatedBox()};
  auto plane = middle();
  plane.origin.z() = 0;
  checkSection(section(input, plane), 200, 1);
  plane.origin.z() = 6;
  checkSection(section(input, plane), 0, 0);
  plane.normal = Vector3d(0, 0, -1);
  checkSection(section(input, plane), 200, 1);
  plane.origin.z() = 0;
  checkSection(section(input, plane), 0, 0);
  plane = middle();
  plane.origin.z() = 7;
  checkSection(section(input, plane), 0, 0);
  checkSection(section(input, {{0, 0, 0}, {-1, -1, -1}, {0, 1, 0}}), 0, 0);
}

TEST_CASE("Geometry analysis preserves scaled geometry and reports numeric limits", "[geometry-analysis]")
{
  for (const double scale : {1e-3, 1e3}) {
    Input input{box()};
    input.objectToWorld.scale(scale);
    const auto measured = measureSolid(input);
    INFO(measured.diagnostic.detail);
    REQUIRE(measured.diagnostic.error == Error::None);
    REQUIRE(measured.surfaceArea);
    REQUIRE(measured.volume);
    close(*measured.surfaceArea, 760 * scale * scale);
    close(*measured.volume, 1200 * scale * scale * scale);
    auto plane = middle();
    plane.origin *= scale;
    checkSection(section(input, plane), 200 * scale * scale, 1);
  }
  Input huge{evaluatedBox()};
  huge.objectToWorld.scale(1e12);
  const auto limited = section(huge, {{0, 0, 3e12}, {0, 0, 1}, {0, 1, 0}});
  REQUIRE(limited.diagnostic.error == Error::BackendFailure);
  REQUIRE_FALSE(limited.area);
  REQUIRE_FALSE(limited.sectionToWorld);
  REQUIRE(limited.contours.empty());
}

TEST_CASE("Geometry analysis solid and section failures never publish partial results", "[geometry-analysis]")
{
  auto mesh = box();
  mesh->indices.pop_back();
  const auto cut = section(Input{mesh}, middle());
  REQUIRE(cut.diagnostic.error == Error::InvalidTopology);
  REQUIRE(cut.contours.empty());
  REQUIRE_FALSE(cut.area);
  REQUIRE_FALSE(cut.sectionToWorld);
  auto degenerate = box();
  degenerate->indices[0] = {0, 0, 1};
  REQUIRE(measureSolid(Input{degenerate}).diagnostic.error == Error::InvalidTopology);
  auto inverted = box();
  for (auto& face : inverted->indices) std::reverse(face.begin(), face.end());
  REQUIRE(measureSolid(Input{inverted}).diagnostic.error == Error::InvalidTopology);
  Input input{evaluatedBox()};
  int checks = 0;
  input.limits.cancelled = [&] { ++checks; return false; };
  REQUIRE(measureSolid(input).diagnostic.error == Error::None);
  const int solidChecks = checks;
  checks = 0;
  input.limits.cancelled = [&] { return ++checks == solidChecks; };
  const auto cancelled = measureSolid(input);
  REQUIRE(cancelled.diagnostic.error == Error::Cancelled);
  REQUIRE_FALSE(cancelled.volume);
  REQUIRE_FALSE(cancelled.surfaceArea);
  checks = 0;
  input.limits.cancelled = [&] { ++checks; return false; };
  REQUIRE(section(input, middle()).diagnostic.error == Error::None);
  const int sectionChecks = checks;
  checks = 0;
  input.limits.cancelled = [&] { return ++checks == sectionChecks; };
  const auto cancelledSection = section(input, middle());
  REQUIRE(cancelledSection.diagnostic.error == Error::Cancelled);
  REQUIRE(cancelledSection.contours.empty());
  REQUIRE_FALSE(cancelledSection.area);
  REQUIRE_FALSE(cancelledSection.sectionToWorld);
  input.limits = {};
  input.limits.maxTriangles = 11;
  REQUIRE(measureSolid(input).diagnostic.error == Error::LimitExceeded);
  REQUIRE(section(input, middle()).diagnostic.error == Error::LimitExceeded);
}
#endif

#if defined(ENABLE_CGAL) && defined(ENABLE_MANIFOLD)
TEST_CASE("Geometry analysis accepts evaluated CGAL box conversion", "[geometry-analysis]")
{
  const std::shared_ptr<const Geometry> geometry = CGALUtils::createNefPolyhedronFromPolySet(*box());
  REQUIRE(geometry);
  const auto result = measureSolid(Input{geometry});
  INFO(result.diagnostic.detail);
  REQUIRE(result.diagnostic.error == Error::None);
  REQUIRE(result.volume);
  REQUIRE(result.surfaceArea);
  close(*result.volume, 1200);
  close(*result.surfaceArea, 760);
  checkSection(section(Input{geometry}, middle()), 200, 1);
  const auto large = box();
  for (auto& vertex : large->vertices) vertex.x() += 1e9;
  const std::shared_ptr<const Geometry> translated = CGALUtils::createNefPolyhedronFromPolySet(*large);
  REQUIRE(translated);
  const auto rejected = measureSolid(Input{translated});
  REQUIRE(rejected.diagnostic.error == Error::BackendFailure);
  REQUIRE(rejected.diagnostic.detail.find("float precision") != std::string::npos);
  REQUIRE_FALSE(rejected.volume);
}
#endif

#ifndef ENABLE_MANIFOLD
TEST_CASE("Geometry analysis without Manifold keeps bounds and reports unavailable operations", "[geometry-analysis]")
{
  Input input{box()};
  REQUIRE(bounds(input).diagnostic.error == Error::None);
  const auto solid = measureSolid(input);
  REQUIRE(solid.diagnostic.error == Error::BackendUnavailable);
  REQUIRE_FALSE(solid.volume);
  const auto cut = section(input, middle());
  REQUIRE(cut.diagnostic.error == Error::BackendUnavailable);
  REQUIRE(cut.contours.empty());
  REQUIRE_FALSE(cut.sectionToWorld);
}
#endif
