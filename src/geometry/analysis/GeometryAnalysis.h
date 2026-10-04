#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "geometry/linalg.h"

class Geometry;

// Measurements of evaluated geometry in caller units. No document or GUI state is read.
namespace GeometryAnalysis {

enum class Error {
  None, InvalidInput, UnsupportedGeometry, InvalidTopology,
  BackendUnavailable, LimitExceeded, Cancelled, BackendFailure
};
struct Diagnostic {
  Error error = Error::None;
  std::string detail;
  std::string method;
  std::optional<double> backendTolerance;
};
struct Limits {
  size_t maxVertices = 0;
  size_t maxTriangles = 0;
  std::function<bool()> cancelled;
};
struct Input {
  std::shared_ptr<const Geometry> geometry;
  Transform3d objectToWorld = Transform3d::Identity();
  Limits limits;
};
struct Plane { Vector3d origin, normal, up; };
struct BoundsResult {
  Diagnostic diagnostic;
  std::optional<BoundingBox> bounds;
};
struct SolidResult {
  Diagnostic diagnostic;
  std::optional<double> surfaceArea, volume;
};
struct Contour {
  std::vector<Vector2d> points;
  bool hole = false;
};
struct SectionResult {
  Diagnostic diagnostic;
  std::optional<Transform3d> sectionToWorld;
  std::vector<Contour> contours;
  std::optional<double> area;
};

BoundsResult bounds(const Input&);
SolidResult measureSolid(const Input&);
SectionResult section(const Input&, const Plane&);

}  // namespace GeometryAnalysis
