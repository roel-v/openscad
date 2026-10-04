# Native Customizer attributes

This SCADMaxx variant of OpenSCAD PR #6642 uses the fixed-member subset of OEP8
object-literal notation. Prefix the next variable assignment with an attribute block:

```scad
@{
    group: "Geometry",
    description: "Cube size",
    range: [1:100],
    hidden: mode != "advanced",
    locked: readonly,
}
size = 10;
```

Keys are identifiers, values are OpenSCAD expressions, and commas separate members.
An empty block and a trailing comma are allowed. The previous `key = value;` syntax
is no longer accepted. Annotation blocks do not admit computed keys, quoted keys,
`each`, or object comprehensions. These restrictions do not affect ordinary OEP8
objects elsewhere in the program.

Supported keys are `group`, `description` (alias `desc`), `range`, `min`, `max`,
`step`, `slider`, `hidden`, and `locked`. The existing PR semantics remain:
`hidden` and `locked` expressions are reevaluated when Customizer parameters change;
the other attributes configure the widget initially. Fixed keys do not require
literal values. Unknown attribute names are diagnosed by the Customizer.

The block attaches to the immediately following assignment. It is not itself a
general expression or a variable containing an object. Supporting arbitrary object
expressions as metadata would require separate evaluation/dependency semantics.

References: [PR #6642](https://github.com/openscad/openscad/pull/6642) and
[OEP8a implementation #6081](https://github.com/openscad/openscad/pull/6081).
