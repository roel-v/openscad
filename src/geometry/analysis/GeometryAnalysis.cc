#include "geometry/analysis/GeometryAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <set>
#include <utility>

#include "geometry/Geometry.h"
#include "geometry/PolySet.h"
#include "geometry/PolySetUtils.h"
#ifdef ENABLE_CGAL
#include "geometry/cgal/CGALNefGeometry.h"
#include "geometry/cgal/CGALUtils.h"
#endif
#ifdef ENABLE_MANIFOLD
#include "geometry/ClipperUtils.h"
#include "geometry/manifold/ManifoldGeometry.h"
#include "geometry/manifold/ManifoldUtils.h"
#endif

namespace GeometryAnalysis {
namespace {

struct Failure { Error error; std::string detail; };
[[noreturn]] void fail(Error error, const std::string& detail) { throw Failure{error, detail}; }

void checkpoint(const Limits& limits)
{
  if (limits.cancelled && limits.cancelled()) fail(Error::Cancelled, "Analysis cancelled.");
}

void checkCounts(size_t vertices, size_t triangles, const Limits& limits)
{
  checkpoint(limits);
  if ((limits.maxVertices && vertices > limits.maxVertices) ||
      (limits.maxTriangles && triangles > limits.maxTriangles)) {
    fail(Error::LimitExceeded, "Geometry exceeds caller vertex or triangle limit.");
  }
  if (vertices > std::numeric_limits<size_t>::max() / 3 ||
      triangles > std::numeric_limits<size_t>::max() / 3) {
    fail(Error::LimitExceeded, "Geometry buffer size overflow.");
  }
}

void validateTransform(const Transform3d& transform)
{
  const auto& m = transform.matrix();
  if (!m.allFinite() || m(3, 0) != 0 || m(3, 1) != 0 || m(3, 2) != 0 || m(3, 3) != 1) {
    fail(Error::InvalidInput, "Transform must be finite and affine.");
  }
  const double determinant = transform.linear().determinant();
  if (!std::isfinite(determinant) || determinant == 0) {
    fail(Error::InvalidInput, "Transform is singular or has a nonfinite determinant.");
  }
}

void validateInput(const Input& input)
{
  checkpoint(input.limits);
  if (!input.geometry) fail(Error::InvalidInput, "Geometry is null.");
  if (dynamic_cast<const GeometryList *>(input.geometry.get())) {
    fail(Error::UnsupportedGeometry, "Resolve GeometryList to an individual evaluated solid first.");
  }
  if (input.geometry->getDimension() != 3) fail(Error::UnsupportedGeometry, "Geometry must be 3D.");
  validateTransform(input.objectToWorld);
}

std::shared_ptr<const PolySet> polySet(const Input& input)
{
  if (auto poly = std::dynamic_pointer_cast<const PolySet>(input.geometry)) return poly;
#ifdef ENABLE_CGAL
  if (auto nef = std::dynamic_pointer_cast<const CGALNefGeometry>(input.geometry)) {
    checkpoint(input.limits);
    if (nef->isEmpty()) return PolySet::createEmpty();
    checkCounts(nef->p3->number_of_vertices(), 0, input.limits);
    std::unique_ptr<PolySet> converted;
    try {
      converted = CGALUtils::createPolySetFromNefPolyhedron3(*nef->p3,
        CGALUtils::NefPolySetConversion::StrictAnalysis);
    } catch (const CGALUtils::NefPolySetConversionError& error) {
      fail(error.reason == CGALUtils::NefPolySetConversionError::Reason::InvalidTopology ?
        Error::InvalidTopology : Error::BackendFailure, error.what());
    }
    checkpoint(input.limits);
    if (!converted) fail(Error::BackendFailure, "CGAL-to-PolySet conversion failed.");
    return converted;
  }
#endif
  fail(Error::UnsupportedGeometry, "Unsupported evaluated geometry type.");
}

void validatePolySet(const PolySet& poly, const Limits& limits)
{
  checkCounts(poly.vertices.size(), poly.indices.size(), limits);
  for (const auto& vertex : poly.vertices) {
    checkpoint(limits);
    if (!vertex.allFinite()) fail(Error::InvalidInput, "PolySet contains a nonfinite vertex.");
  }
  size_t triangleCount = 0;
  for (const auto& face : poly.indices) {
    checkpoint(limits);
    if (face.size() < 3 || (poly.isTriangular() && face.size() != 3)) {
      fail(Error::InvalidTopology, "PolySet contains an invalid face size.");
    }
    if (face.size() - 2 > std::numeric_limits<size_t>::max() - triangleCount) {
      fail(Error::LimitExceeded, "Triangulated face count overflow.");
    }
    triangleCount += face.size() - 2;
    for (const auto index : face) {
      checkpoint(limits);
      if (index < 0 || static_cast<size_t>(index) >= poly.vertices.size()) {
        fail(Error::InvalidTopology, "PolySet face index is out of range.");
      }
    }
  }
  checkCounts(poly.vertices.size(), triangleCount, limits);
}

Transform3d sectionFrame(const Plane& plane)
{
  if (!plane.origin.allFinite() || !plane.normal.allFinite() || !plane.up.allFinite()) {
    fail(Error::InvalidInput, "Section origin, normal and up must be finite.");
  }
  const double normalLength = plane.normal.stableNorm(), upLength = plane.up.stableNorm();
  if (!std::isfinite(normalLength) || !std::isfinite(upLength) ||
      normalLength <= 1e-12 || upLength <= 1e-12) {
    fail(Error::InvalidInput, "Section normal and up must have nonzero length.");
  }
  const Vector3d normal = plane.normal / normalLength, up = plane.up / upLength;
  const Vector3d perpendicular = up - normal * up.dot(normal);
  const double length = perpendicular.stableNorm();
  if (length < 1e-8) fail(Error::InvalidInput, "Section up is parallel or nearly parallel to normal.");
  const Vector3d y = perpendicular / length, x = y.cross(normal);
  Transform3d frame = Transform3d::Identity();
  frame.linear().col(0) = x;
  frame.linear().col(1) = y;
  frame.linear().col(2) = normal;
  frame.translation() = plane.origin;
  return frame;
}

#ifdef ENABLE_MANIFOLD
void validateManifold(const ManifoldGeometry& geometry, Error error)
{
  const auto& solid = geometry.getManifold();
  if (solid.Status() != manifold::Manifold::Error::NoError) {
    fail(error, std::string("Manifold status: ") + ManifoldUtils::statusToString(solid.Status()) +
      ". Strict analysis does not merge disconnected indices or repair meshes.");
  }
  if (!geometry.isEmpty()) {
    const double volume = solid.Volume();
    if (!std::isfinite(volume)) fail(Error::BackendFailure, "Manifold volume is nonfinite.");
    if (volume <= 0) fail(Error::InvalidTopology, "Solid has degenerate or negative oriented volume.");
  }
}

ManifoldGeometry prepareSolid(const Input& input, const Transform3d& transform)
{
  validateTransform(transform);
  ManifoldGeometry prepared;
  if (auto existing = std::dynamic_pointer_cast<const ManifoldGeometry>(input.geometry)) {
    checkCounts(existing->numVertices(), existing->numFacets(), input.limits);
    validateManifold(*existing, Error::InvalidTopology);
    prepared = *existing;
  } else {
    const auto poly = polySet(input);
    validatePolySet(*poly, input.limits);
    size_t expectedTriangles = 0;
    for (const auto& face : poly->indices) {
      checkpoint(input.limits);
      expectedTriangles += face.size() - 2; // Overflow was checked by validatePolySet.
      std::set<size_t> used;
      Vector3d normal = Vector3d::Zero();
      for (size_t j = 0; j < face.size(); ++j) {
        checkpoint(input.limits);
        if (!used.insert(face[j]).second ||
            poly->vertices[face[j]] == poly->vertices[face[(j + 1) % face.size()]]) {
          fail(Error::InvalidTopology, "Face contains repeated vertices or a zero-length edge.");
        }
        if (!poly->isTriangular() && poly->vertices[face[j]].cast<float>() ==
            poly->vertices[face[(j + 1) % face.size()]].cast<float>()) {
          fail(Error::BackendFailure, "Distinct face vertices collapse at triangulator float precision.");
        }
        if (j > 0 && j + 1 < face.size()) {
          normal += (poly->vertices[face[j]] - poly->vertices[face[0]])
            .cross(poly->vertices[face[j + 1]] - poly->vertices[face[0]]);
        }
      }
      if (!normal.allFinite()) fail(Error::BackendFailure, "Face normal overflows numeric representation.");
      if (normal.stableNorm() == 0) fail(Error::InvalidTopology, "Face has zero area.");
    }
    // Strip rendering metadata so the triangulator cannot follow unrelated color indices.
    PolySet copy = *poly;
    copy.colors.clear();
    copy.color_indices.clear();
    checkpoint(input.limits);
    const auto triangles = PolySetUtils::tessellate_faces(copy);
    checkpoint(input.limits);
    if (!triangles) fail(Error::BackendFailure, "Face triangulation failed.");
    validatePolySet(*triangles, input.limits);
    if (triangles->indices.size() != expectedTriangles) {
      fail(Error::BackendFailure, "Triangulation dropped or changed faces; strict analysis refuses simplification.");
    }
    if (!poly->isEmpty() && triangles->isEmpty()) fail(Error::InvalidTopology, "Face triangulation produced no triangles.");
    manifold::MeshGL64 mesh;
    mesh.numProp = 3;
    mesh.vertProperties.reserve(triangles->vertices.size() * 3);
    mesh.triVerts.reserve(triangles->indices.size() * 3);
    for (const auto& vertex : triangles->vertices) {
      checkpoint(input.limits);
      mesh.vertProperties.insert(mesh.vertProperties.end(), {vertex.x(), vertex.y(), vertex.z()});
    }
    for (const auto& face : triangles->indices) {
      checkpoint(input.limits);
      const auto& a = triangles->vertices[face[0]];
      const auto& b = triangles->vertices[face[1]];
      const auto& c = triangles->vertices[face[2]];
      const Vector3d normal = (b - a).cross(c - a);
      if (!normal.allFinite()) fail(Error::BackendFailure, "Triangle normal overflows numeric representation.");
      if (normal.stableNorm() == 0) fail(Error::InvalidTopology, "Triangulation contains a zero-area triangle.");
      for (const auto index : face) mesh.triVerts.push_back(static_cast<uint64_t>(index));
    }
    checkpoint(input.limits);
    prepared = ManifoldGeometry(manifold::Manifold(mesh));
    checkpoint(input.limits);
    validateManifold(prepared, Error::InvalidTopology);
  }
  checkpoint(input.limits);
  prepared.transform(transform);
  checkpoint(input.limits);
  validateManifold(prepared, Error::BackendFailure);
  // Lazy transforms can overflow despite finite input coordinates and matrix elements.
  prepared.foreachVertexUntilTrue([&](const manifold::vec3& vertex) {
    checkpoint(input.limits);
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
      fail(Error::BackendFailure, "Transformed Manifold vertex is nonfinite.");
    }
    return false;
  });
  return prepared;
}

std::optional<double> backendTolerance(const ManifoldGeometry& geometry)
{
  // Empty Manifold has no meaningful scale/tolerance (the backend uses a negative sentinel).
  if (geometry.isEmpty()) return std::nullopt;
  const double tolerance = geometry.getManifold().GetTolerance();
  if (!std::isfinite(tolerance) || tolerance < 0) {
    fail(Error::BackendFailure, "Manifold tolerance is nonfinite or negative.");
  }
  return tolerance;
}

bool pointLess(const Vector2d& a, const Vector2d& b)
{
  return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
}

double contourArea(const Contour& contour)
{
  long double twiceArea = 0;
  const auto& anchor = contour.points.front();
  for (size_t j = 0; j < contour.points.size(); ++j) {
    const Vector2d a = contour.points[j] - anchor;
    const Vector2d b = contour.points[(j + 1) % contour.points.size()] - anchor;
    twiceArea += static_cast<long double>(a.x()) * b.y() - static_cast<long double>(b.x()) * a.y();
  }
  const double area = static_cast<double>(twiceArea / 2);
  if (!std::isfinite(area) || (contour.hole ? area > 0 : area < 0)) {
    fail(Error::BackendFailure, "Sanitized contour area or hole winding is invalid.");
  }
  return area;
}
#endif

template <typename Result, typename Operation>
Result run(const char *method, Operation operation)
{
  Result result;
  result.diagnostic.method = method;
  try {
    operation(result);
    return result;
  } catch (const Failure& failure) {
    Result failed;
    failed.diagnostic = {failure.error, failure.detail, method, std::nullopt};
    return failed;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& exception) {
    Result failed;
    failed.diagnostic = {Error::BackendFailure, exception.what(), method, std::nullopt};
    return failed;
  }
}

}  // namespace

BoundsResult bounds(const Input& input)
{
  return run<BoundsResult>("mesh-bounds", [&](BoundsResult& result) {
    validateInput(input);
    BoundingBox box;
    const auto extend = [&](const Vector3d& vertex) {
      checkpoint(input.limits);
      const Vector3d world = input.objectToWorld * vertex;
      if (!world.allFinite()) fail(Error::InvalidInput, "Transformed vertex is nonfinite.");
      box.extend(world);
    };
#ifdef ENABLE_MANIFOLD
    if (auto manifold = std::dynamic_pointer_cast<const ManifoldGeometry>(input.geometry)) {
      checkCounts(manifold->numVertices(), manifold->numFacets(), input.limits);
      if (!manifold->isValid()) fail(Error::InvalidTopology, "Manifold geometry has invalid backend status.");
      manifold->foreachVertexUntilTrue([&](const manifold::vec3& v) {
        extend(Vector3d(v.x, v.y, v.z));
        return false;
      });
    } else
#endif
    {
      const auto poly = polySet(input);
      validatePolySet(*poly, input.limits);
      for (const auto& face : poly->indices) {
        for (const auto index : face) extend(poly->vertices[index]);
      }
    }
    checkpoint(input.limits);
    if (!box.isEmpty()) result.bounds = box;
  });
}

SolidResult measureSolid(const Input& input)
{
  return run<SolidResult>("manifold-solid", [&](SolidResult& result) {
    validateInput(input);
#ifdef ENABLE_MANIFOLD
    const auto prepared = prepareSolid(input, input.objectToWorld);
    checkpoint(input.limits);
    const double area = prepared.getManifold().SurfaceArea();
    checkpoint(input.limits);
    const double volume = prepared.getManifold().Volume();
    const auto tolerance = backendTolerance(prepared);
    if (!std::isfinite(area) || !std::isfinite(volume) || area < 0 || volume < 0) {
      fail(Error::BackendFailure, "Manifold area or volume is nonfinite or negative.");
    }
    checkpoint(input.limits);
    result.surfaceArea = area;
    result.volume = volume;
    result.diagnostic.backendTolerance = tolerance;
#else
    fail(Error::BackendUnavailable, "Solid analysis requires ENABLE_MANIFOLD.");
#endif
  });
}

SectionResult section(const Input& input, const Plane& plane)
{
  return run<SectionResult>("manifold-section", [&](SectionResult& result) {
    validateInput(input);
    const auto frame = sectionFrame(plane);
#ifdef ENABLE_MANIFOLD
    const auto prepared = prepareSolid(input, frame.inverse() * input.objectToWorld);
    const auto tolerance = backendTolerance(prepared);
    checkpoint(input.limits);
    const auto raw = prepared.slice();
    checkpoint(input.limits);
    validateManifold(prepared, Error::BackendFailure);
    const double coordinateLimit = std::ldexp(static_cast<double>(Clipper2Lib::MAX_COORD),
      -ClipperUtils::scaleBitsFromPrecision());
    for (const auto& outline : raw.outlines()) {
      for (const auto& point : outline.vertices) {
        checkpoint(input.limits);
        if (!point.allFinite() || point.cwiseAbs().maxCoeff() >= coordinateLimit) {
          fail(Error::BackendFailure, "Section exceeds finite Clipper sanitization coordinate range.");
        }
      }
    }
    const auto sanitized = ClipperUtils::sanitize(raw);
    checkpoint(input.limits);
    if (!sanitized) fail(Error::BackendFailure, "Section contour sanitization failed.");
    std::vector<Contour> contours;
    long double netArea = 0;
    for (const auto& outline : sanitized->outlines()) {
      Contour contour;
      contour.hole = !outline.positive;
      for (const auto& point : outline.vertices) {
        checkpoint(input.limits);
        if (!point.allFinite()) fail(Error::BackendFailure, "Sanitized section coordinate is nonfinite.");
        contour.points.push_back(point);
      }
      while (contour.points.size() > 1 && contour.points.front() == contour.points.back()) {
        contour.points.pop_back();
      }
      if (contour.points.size() < 3) fail(Error::BackendFailure, "Sanitized contour has fewer than three points.");
      netArea += contourArea(contour);
      const auto first = std::min_element(contour.points.begin(), contour.points.end(), pointLess);
      std::rotate(contour.points.begin(), first, contour.points.end());
      contours.push_back(std::move(contour));
    }
    std::sort(contours.begin(), contours.end(), [](const Contour& a, const Contour& b) {
      if (a.hole != b.hole) return a.hole < b.hole;
      return std::lexicographical_compare(a.points.begin(), a.points.end(),
        b.points.begin(), b.points.end(), pointLess);
    });
    const double area = static_cast<double>(netArea);
    if (!std::isfinite(area) || area < 0) fail(Error::BackendFailure, "Net section area is nonfinite or negative.");
    checkpoint(input.limits);
    result.sectionToWorld = frame;
    result.contours = std::move(contours);
    result.area = area;
    result.diagnostic.backendTolerance = tolerance;
#else
    fail(Error::BackendUnavailable, "Section analysis requires ENABLE_MANIFOLD.");
#endif
  });
}
}  // namespace GeometryAnalysis
