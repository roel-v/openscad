# Live Sections

Live clipping consumes the shared immutable inspection mesh presentation and canonical `Section`
design records. It does not modify solids or rerun SCAD source while a plane moves.

Open **Window > Sections** to show the dock. New XY/XZ/YZ creates a transient Section at
the model bounds center. Name it, enable it, and use the model-range slider or numeric
offset to move along its current normal. Numeric offsets may be negative or outside
the model range. Origin X/Y/Z sets its world position through the resolved design frame;
azimuth and elevation represent its current normal. Flip and Reset keep the origin fixed.
Show selected plane helper displays the plane outline, origin, retained-side arrow and
two rotation rings. Helper visibility is independent of clipping enablement.

**Pick point** preserves orientation. **Pick + align** uses the negative outward surface
normal so the clicked boundary is removed and the interior behind it becomes visible.
A miss keeps picking active; Escape cancels it. Drag the origin or normal arrow to adjust
offset, or a rotation ring to rotate about the origin. Shift snaps rotation to 15 degrees.
A nearly parallel handle needs a usable screen projection; otherwise change the camera
or use numeric controls. Camera changes, viewport resize, replacement tools, source edits,
tab changes and owner closure cancel an unfinished gesture.

Each gesture previews one shared design-object draft. Absolute built-in field updates
replace the earlier update of the same field in that draft; registered custom consumers
retain their ordered change stream. Release commits one editor undo action. Source-backed
edits change bounded `adjust` fields and preserve expressions such as `origin:[0,0,height/2]`.
The resulting source change invalidates the evaluated result; use ordinary Preview/Render
to evaluate that changed source. An unavailable or stale definition disables graphical edits
with feedback. Computed/helper-valued adjustment fields require direct source editing.

**Promote to source** writes a transient Section through the common design-object lifecycle
and keeps its stable ID. Ordinary **Save** uses the editor's conflict handling. Reopening
source reconstructs the same named Section; personal settings store only enabled IDs,
helper IDs, selection and the global toggle. There is no separate Section-definition file.
Deleting a source Section removes its declaration in one undo action and can leave another
design object with a Missing reference. No replacement definition is invented.

The renderer intersects up to eight enabled positive half-spaces. A fragment is retained only
when every normalized plane satisfies `(worldPosition - origin) dot normal > epsilon`.
Color, edge, depth and both surface-pick passes use the same `ClipDiscard.glsl` predicate.
Boundary fragments are discarded. Culling is disabled so an exposed interior or rear surface
can remain visible. No cap triangles are generated.

Mesh positions and plane constants share the mesh bounds center. The plane constant is computed
in double precision before the recentered value is sent to the shader. The tolerance is
`max(8 * FLT_EPSILON * boundsDiagonal, 64 * DBL_EPSILON * max(boundsDiagonal, |boundsCenter|, |origin|))`.
Empty, nonfinite or degenerate bounds cannot produce an enabled plane. The tolerance is never
increased to conceal a failed thin-wall sample.
CGAL conversion and nontriangular PolySet tessellation also use a private recentered copy
before their native float indexing; the corresponding piece transform restores the world
position. Recentring only at vertex upload would be too late for those conversion steps.

The priority-zero root provider retains an evaluated scene and an independent `Tree`, waits for
the ordinary compile/cache lock, and prepares the native preview tree. Its instantiated `$preview`
and parameter values therefore remain those of that scene. Current final geometry is converted
without another tree evaluation. Three-dimensional convertible meshes are supported; 2D,
Thrown Together and nonconvertible geometry report an explicit unsupported status. A higher
provider owns its pieces, IDs, transforms, colors, roles and masks even if preparation fails or
remains pending. The root provider never substitutes a merged fallback under that ownership.

Cancellation uses a worker-local progress callback and retains the scene until work finishes.
Delivery is queued to the GUI and the shared coordinator rejects obsolete result tickets.
Changing an enabled plane updates uniforms and the presentation serial only. It does not create
a mesh job or upload another vertex buffer.

The native `TestClipping` harness records visible retained surfaces separately from world-space
pick points and normals. It also records preparation time and p50/p95 frame times for a mesh of
at least 100,000 triangles at 1280 by 720 physical pixels, with one and eight enabled planes.
These measurements apply to the recorded GPU and driver; they do not establish a portable GPU
performance guarantee. Computed contours and section export belong to the separate geometry
service feature.

Native numerical verification exercises frozen preview, actual Manifold final geometry and
actual convertible CGAL final geometry at scales 1e-4, 1 and 1e4, oblique orientations,
mirrors, and translated centers of up to 1e5 scale units. Each uses one and eight planes,
discards the coincident boundary, retains a separate wall at 1e-4 of the world bounds
diagonal, and checks visible pixels and world-space depth/normal separately. Provider-owned
piece transforms, 64-bit IDs, masks and background/highlight roles remain intact. Helpers
are drawn after the model and never enter the surface-pick buffers or the discard predicate.

The tolerance represents a finite floating-point precision limit. Smaller separations or
larger translations outside the verified range need their own evidence. Caps are explicitly
unavailable. The Windows extracted-package checks cover the recorded Qt/GPU machine and
100%, 150% and 200% scaling; other platforms and a clean machine remain unverified.
