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

## Attributes and evaluation

The following describes the SCADMaxx variant at source revision
`8ea44273775f3c7720f2ff607e6d4ea5215422a6`, inspected on 2026-10-02.
Semantics are implemented in `src/core/customizer/ParameterObject.cc`,
`src/core/customizer/ParameterObject.h` and `src/gui/parameter/ParameterWidget.cc`.
Keep this reference with changes to those implementations.

| Attribute | Intended value and effect | Default | Evaluation |
| --- | --- | --- | --- |
| `group` | String naming the parameter group | `"Parameters"` | Initialization |
| `description`, `desc` | String used as descriptive text | Empty | Initialization |
| `hidden` | Boolean; omit the parameter widget when true | `false` | Initialization and reactive updates |
| `locked` | Boolean; disable editing when true | `false` | Initialization and reactive updates |
| `range` | Literal forms described below; limits or choices for the widget | No explicit limits/choices | Initialization, parsed from the expression AST |
| `min` | Number; override the minimum of a scalar number widget | Unspecified | Initialization |
| `max` | Number; override the maximum of a scalar number widget | Unspecified | Initialization |
| `step` | Positive number; override the step of a scalar number widget | Widget-derived if unspecified | Initialization |
| `slider` | Boolean; permit a scalar numeric slider when both bounds exist; false selects spinbox | `true` | Initialization |

Use the stated types. Implementation converts group/description values to strings, hidden/locked/slider
to Boolean, and min/max/step to double; those conversions are not a substitute for meaningful values.
Unresolved initial values leave defaults. Unknown keys with defined values emit a Customizer error;
an unresolved value can prevent that diagnostic. Do not rely on diagnostics to validate a spelling.
Avoid duplicate keys or simultaneous `desc` and `description`.

Native metadata takes precedence over comment metadata on the same assignment. The parameter default
must be a supported literal: Boolean, number, string, or a numeric vector of one to four components.
Annotating an arbitrary computed assignment does not make it an editable parameter.
`min`, `max`, `step` and `slider` overrides apply to scalar number widgets, not enumeration, string,
Boolean or vector widgets. A slider needs both bounds; merely setting `slider:true` supplies neither.
Without an explicit step the GUI derives a step from decimal precision; do not assume a universal step of 1.
Unset bounds depend on the widget (including a zero opposite bound in some one-sided cases).

### Literal `range` forms

- Number or numeric vector default: `[minimum:maximum]` or `[minimum:step:maximum]`, with literal numeric
  components, sets bounds and optional step. `[maximum]` sets only the maximum. A number such as `0.1`
  sets a step. Keep defaults within the limits; parsed limits can widen to include defaults with warnings.
- Number or string default: a vector of literal choices, such as `["small","large"]` or `[10,20,30]`,
  produces an enumeration. `[value,label]` pairs supply display labels, e.g. `[[10,"Small"],[20,"Large"]]`.
  A singleton numeric vector has the maximum-bound meaning above, not a one-choice enumeration.
  If absent from the choices, the default is added by the implementation.
- String default: a literal number supplies a maximum length; a shorter maximum is widened to accommodate
  the default. Use a sensible positive integer.

Although the grammar permits expressions, `range` is interpreted structurally by the widget constructors.
Do not expect `range: choices_variable`, computed range endpoints or function-generated choice lists to
behave like the corresponding literal AST. Use literal `range` forms. Scalar `min`, `max` and `step` can
evaluate expressions initially, then remain fixed until the widget metadata is rebuilt.

### Reactive values and includes

`hidden` and `locked` retain their expressions and reevaluate with the current Customizer context.
Other attributes initialize metadata; they do not continuously track parameter changes. Dependencies
may include ordinary parameters and special variables. Their values must exist in the relevant context;
do not assume `$preview` has the same value for preview, final render and every CLI operation.

`model.scad`:

```scad
advanced = false;
readonly = false;
@{group: "Controls", description: "Special-variable lock"}
$detail_lock = false;
include <controls.scad>
cube([20,20,detail]);
```

`controls.scad`:

```scad
@{group: "Detail", range: [1:1:10], hidden: !advanced, locked: readonly || $detail_lock,}
detail = 3;
```

Use `include` for this example, not `use`, which does not import the parameter assignments.
Verify hide/lock changes in the GUI; successful CLI compilation only verifies syntax and geometry.
Hiding an empty group is a separate UI behavior and is not guaranteed by this syntax change.

## Metadata versus ordinary objects

The block attaches to the immediately following assignment. It is not itself a
general expression or a variable containing an object. Supporting arbitrary object
expressions as metadata would require separate evaluation/dependency semantics.

With OEP8 support enabled, ordinary objects can be assigned, accessed and updated:

```scad
base = {width:10, "label text":"cube"};
updated = object(base, width=20);
assert(base.width == 10 && updated.width == 20);
assert(base["label text"] == "cube");
```

Object literals and `object()` require their own build support/feature enablement. Native annotation
syntax does not imply that ordinary object literals exist, or vice versa. These annotations are invalid:

```scad
// @{ "group": "Shape" } size = 10;     // quoted key
// @{ (key): true } size = 10;            // computed key
// @{ each metadata } size = 10;         // object expansion
// @{ for(i=[1:3]) (str(i)): i } x = 1;   // comprehension
// @{ hidden = false; } size = 10;       // removed spelling
```

Replace the old spelling directly: `@{ hidden = false; }` becomes `@{ hidden: false }`.
Do not add an old-syntax fallback. A parser error at `@{` or `:` can mean the wrong executable or an older
variant; check the exact build and run a minimal syntax probe before modifying a model to match it.

References: [PR #6642](https://github.com/openscad/openscad/pull/6642) and
[OEP8a implementation #6081](https://github.com/openscad/openscad/pull/6081).
