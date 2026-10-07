# Geometry values and bounds

Enable `object-function` in an experimental build. `{{ statements }}` captures a geometry value without
displaying it. Module parameters and local assignments are resolved in the current evaluation context;
Boolean geometry is evaluated when `geometry_bounds(shape)` first needs it. Subsequent queries on that
value reuse the result. Geometry values can be passed through functions and stored in vectors or objects.

```scad
shape = {{ translate([10, 0, 0]) cube([40, 30, 20]); }};
bounds = geometry_bounds(shape);
echo(bounds.min, bounds.max);    // [10,0,0], [50,30,20]
echo(bounds.size, bounds.center); // [40,30,20], [30,15,10]
translate([bounds.max.x + 5, 0, 0]) cube(5);
```

The result is an OEP8 object with three-component vectors `min`, `max`, `size`, `center`.
These are axis-aligned bounds of the evaluated result, including captured transforms and Boolean operations.
The center is `(min + max) / 2`, not center of mass. A 2D result has Z zero.
Empty geometry, mixed dimensions and non-finite transforms/bounds produce diagnostics and `undef`.
Wrong arguments are diagnosed. Cancellation/evaluation-limit exceptions propagate normally.

Capturing geometry does not display it. Use an ordinary call to the same module when the model also needs
to display that shape; see `examples/SCADMaxx/geometry-bounds.scad`. Direct geometry-value output, mesh arrays,
module references and hybrid object/geometry literals are outside this feature. Values print as `<geometry>`;
equality is capture identity, not geometric equivalence.

Bounds queries can perform expensive Boolean work during source evaluation. They are not a preview-only
approximation. Strict-query cache entries are separate from permissive render cache entries so a previously
ignored mixed-dimension operand cannot silently produce a misleading box.

Design reference: OpenSCAD PR #4478's geometry literals and geometry query proposal. This implementation
uses the current evaluator and OEP8 objects without importing that broader prototype's mesh extraction.

Implementation is awaiting its first authorized build and test run. The object echo suite discovers
`geometry-bounds-tests.scad`; run it with each available CGAL/Manifold backend. Also check empty literals,
complete subtraction, mixed 2D/3D children, invalid arguments, cancellation, and disabled object-function.
