# Anchors and attachments

The experimental source contract uses typed assignments. An Anchor supplies a point and a complete orientation;
Point, Axis and Plane retain their more limited mathematical meaning. Declarations emit no geometry.

```scad
@{type: Part, id: "plate"}
module plate(width = 40) {
    @{type: Anchor}
    mount = {id: "mount", position: [width/2, 0, 10], normal: [0,0,1], up: [0,1,0]};
    cube([width, 20, 10]);
}
@{type: Component, id: "left"} plate();
@{type: Component, id: "right"} plate(width = 60);
```

The two instances own distinct `mount` references, at local X=20 and X=30. IDs are case-sensitive and belong to the
owner, while labels are display text. An explicit file-scope `owner: "/left"` adds a reference to that instance.
An addition cannot replace an inherited reference with the same ID: both become invalid.

Reference declarations belong directly to a file or an annotated Part/Assembly body. A helper function may return
their value. Nested helper modules, control blocks and transform blocks cannot declare an implicit reference frame.
Part bodies cannot declare Attachments or specify another owner. Top-level `use` library declarations are excluded;
invoked annotated modules evaluate their declarations from the invocation's actual parameters.

Anchor fields are `id`, `position`, `normal`, `up`, with optional `label`, `owner`, `spin`, `capture`, `purpose` and
`adjust`. `adjust.translation` is a local vector; `adjust.rotation` is an xyzw quaternion. `capture` is `parametric`
or `fixed`; a fixed capture follows its owner's placement and retains its saved local coordinates. `purpose:
"drawing"` selects one canonical drawing anchor per owner. Point requires `position`, Axis adds `direction`, and
Plane adds `normal`. All vectors contain three finite numbers. Unknown and duplicate fields are errors, including
fields in objects returned by helper functions.

Attachment objects have `id`, `child: {component, anchor}`, and `target: {component, anchor}`, plus optional `label`,
`mode`, `offset`, `spin`, and `adjust: {offset, spin}`. Endpoints require oriented Anchors. Relative paths belong to
the declaring Assembly; `/` introduces an absolute Component path. Empty means the declaring owner/document.
Path segments escape `~` as `~0` and `/` as `~1`. Traversal and wildcard paths are unsupported.

One relation may drive each child. Relations choose placement without changing semantic containment. Missing
endpoints, invalid relations and multiple drivers fail the new evaluation rather than publishing an unrelated base
placement. Independent declarations may appear before the Component calls they reference.

The supplied `tests/data/scad/anchors/reusable-fixture.scad` contains two independent instances of an Assembly with
plate/peg references and relative Attachment declarations. The peg origins are [20,0,10] and [130,0,10]. Changing
the first width to 50 moves only its peg to [25,0,10]. GUI preview, final rendering, CSG and STL use the same solved
native tree. Local Part export retains its original geometry; local Assembly export includes solved descendants.

Mate opposes the child and target normals with a 180-degree local Y rotation; Align matches their orientations.
Offset is expressed in the target frame, followed by spin around its Z axis. Corrections are rigid in world space,
including beneath scaled, mirrored or sheared owners. Surface normals use the inverse transpose; up vectors use
the owner's linear transform and tangent projection. Singular or excessively ill-conditioned frames are unavailable.
Containment and relation dependencies form one stable ordered graph. Cycles and root modifiers that discard a
required correction abort the new evaluation. Hiding, Solo and inspection effects do not change the graph or export.

The Inspector lists named references, their type, owner, source and world values. Pick anchor captures a current
surface point, normal and camera up into an unsaved local draft. Save anchor inserts a fixed root declaration with
an explicit Component owner, or document ownership for anonymous geometry. Saving does not write the file to disk.
Cancel leaves source untouched. Promotion, undo and redo share the existing document lifecycle.

Label and literal adjustment controls preserve the original position, offset and spin expressions. Editing a
declaration used by several invocations requires Edit shared declaration and displays its actual use count. A helper
RHS or an existing computed adjustment stays available for source editing. Source owners must be explicitly opened,
writable and captured by a current evaluation. Changing source or included inputs makes retained results stale.

`include <SCADMaxx/bosl2-anchors.scad>` exposes
`smx_bosl2_anchor(id, geometry, selector, label=undef, placement=ident(4))`. Install BOSL2 separately on the normal
library search path. This bridge requires version 2.0.766, revision e173fa0ae45f9e2082e0d8c0382dae98621b7c20, recorded
in `libraries/SCADMaxx/bosl2-adapter.json`. It accepts explicit 3D prismoid, conoid, spheroid and point descriptors
from `attach_geom`, with standard or named selectors, and returns ordinary Anchor fields. An explicit placement uses
the same affine normal/up rules as native references. Unsupported descriptors, versions and malformed results report
diagnostics. The application package contains the helper and manifest; BOSL2 remains external. See
`examples/SCADMaxx/bosl2-anchors.scad` for a native Attachment using these values.
