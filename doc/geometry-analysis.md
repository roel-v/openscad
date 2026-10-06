# Geometry analysis

`geometry/analysis/GeometryAnalysis.h` provides stateless bounds, solid measurements and arbitrary-plane sections
of immutable evaluated 3D geometry. It does not look up windows, editors or documents. Callers resolve an individual
target (or an evaluated solid union), keep request/result identities, schedule work and reject stale results.
An unresolved `GeometryList` is rejected rather than implicitly concatenated or unioned.

All lengths use the caller's geometry units. Area and volume use squared and cubed units. The API measures the
evaluated tessellation, not exact analytic source intent. `Diagnostic::method` is `mesh-bounds`, `manifold-solid` or
`manifold-section`. `backendTolerance` reports Manifold's actual tolerance; it is not a universal measurement error
bound. Consumers supply unit labels and correlation IDs.

## Input and errors

The object-to-world transform must be finite, affine and nonsingular. Bounds use the actual transformed referenced
vertices, including those of finite open meshes; unused PolySet vertices cannot enlarge the box. Valid empty geometry
returns success with no bounds. Solid area/volume and sections require valid closed geometry. Empty supported solids
have zero area/volume, and empty intersections retain a valid section frame with no contours and area zero.

Supported types are indexed `PolySet`, evaluated `ManifoldGeometry`, and `CGALNefGeometry` when CGAL is enabled.
CGAL conversion uses `StrictAnalysis` mode of the existing CGAL-to-PolySet path, followed by the same strict solid check.
That mode validates bounded/simple Nef geometry, skips global render colors, retains double output coordinates and
rejects distinct exact vertices that share a float triangulator key, changed faces and invalid connectivity.
The public header has no
Qt or vendor types. Without Manifold, PolySet bounds still work and solid/section operations return `BackendUnavailable`.

Strict conversion validates finite coordinates, indices, face sizes, nonzero faces, triangle counts and backend
status. A false `PolySet::isManifold()` flag means unknown; a valid indexed cube is still accepted. Conversion never
calls Merge, welding, hull, resampling or CGAL repair. Disconnected triangle indices can therefore fail even when their
coordinate positions describe a closed surface. The nontriangle tessellator uses float coordinates; distinct edges
that collapse at that precision, dropped faces and degenerate output triangles are explicitly rejected. Triangular
inputs retain the double-precision fast path. Validity and numeric limits still depend on Manifold's evaluated mesh
contract; this API does not certify exact analytic geometry.

For example, a cube missing one triangle returns `InvalidTopology`, with backend status in the detail when Manifold
rejects it, and with no area or volume payload. Nonfinite inputs, unsupported/2D targets, singular transforms, caller
limits, cancellation, unavailable backends and numeric/backend failures have typed errors. No failure returns a
plausible zero measurement, partial contours or a partial frame.

## Section frames and boundary convention

A `Plane` specifies a world origin, normal and drawing-up vector. Directions must be finite and have length greater
than 1e-12. After normalization, up's component perpendicular to the normal must have length at least 1e-8. Let N be
the unit normal, Y the normalized perpendicular up, and X = Y cross N. The returned frame is right-handed:
X cross Y = N. `sectionToWorld` maps (x,y,0) into the requested world plane; its inverse maps world coordinates into
section coordinates. For normal +Z and up +Y, the axes are world +X and +Y.

Manifold slicing happens at section z=0, without offsetting the requested plane. It includes the minimum-Z boundary
and excludes the maximum-Z boundary. Reversing the normal can reverse which coincident boundary is included. A
tangent point/edge or an outside plane has no area. This numerical convention is separate from display clipping.

Contours are sanitized with the existing Clipper utility, retain holes and disconnected islands, and omit the
duplicated closing vertex. Positive winding describes outer contours and negative winding describes holes. Net area
is computed from those sanitized contours by signed shoelace sums, independent of global rendering settings.
Each loop starts at its lexicographically smallest vertex without changing winding; loops are sorted by hole flag
and coordinates. This is deterministic representation, not persistent topology naming.

Clipper precision 8 corresponds to a 2^-27 coordinate grid in geometry units in the current backend. Sanitization
can remove subgrid detail. Coordinates outside its integer range are rejected before conversion. Manifold tolerance,
tessellation, triangulator float precision for polygonal faces and Clipper quantization all limit accuracy. Numeric
fixture tolerances are evidence for those fixtures, not a universal guarantee.

```cpp
GeometryAnalysis::Input input{evaluatedBox}; // [0,20] x [0,10] x [0,6]
GeometryAnalysis::Plane plane{{0, 0, 3}, {0, 0, 1}, {0, 1, 0}};
auto result = GeometryAnalysis::section(input, plane);
// Success: one outer contour, net area 200, X/Y = world X/Y.
// result.sectionToWorld maps (0,0,0) to world (0,0,3).
```

The same box measures volume 1200 and surface area 760. Section z=0 has area 200; z=6 is empty under the half-open
boundary convention. A through-hole [5,15] x [3,7] removes area 40 from the middle section, yielding one outer contour,
one hole and net area 160.

## Limits and cancellation

Nonzero `maxVertices` and `maxTriangles` impose caller limits, checked before expensive conversion when counts are
available and again after triangulation. Zero means no caller-imposed limit. Overflow and nonfinite backend results
are failures independently of caller limits. Cancellation is checked before conversion, within vertex/face/triangle
loops, between synchronous backend stages and before publishing a result. It cannot interrupt a synchronous backend
call internally. A cancelled operation publishes no measurement or contour payload. Inputs are never mutated.

The explicitly registered Catch cases use the `geometry-analysis` CTest label. Verify discovery before a filtered
run. Existing projection/export behavior remains in its original code; consumers should extend this service rather
than copy its slicing implementation into UI or HTTP code. Empty Manifold geometry has no meaningful backend tolerance;
its negative sentinel is represented by an absent optional tolerance.
