# Semantic models

Part and Assembly annotate ordinary module definitions. Component annotates an
explicit call to one of these templates. Declaring a template produces no geometry.

```scad
@{type:Part, id:"bracket", label:"Bracket"}
module bracket(width=30) {
  difference() {
    cube([width, 10, 3]);
    translate([5, 5, -1]) cylinder(d=3, h=5, $fn=24);
  }
}

@{type:Assembly, id:"pair", label:"Bracket pair"}
module pair(width=35, gap=60) {
  @{type:Component, id:"left"} bracket(width);
  translate([gap, 0, 0])
    @{type:Component, id:"right"} bracket(width+10);
}

@{type:Component, id:"rack"} pair();
```

Definition IDs are unique throughout the document, including consumed libraries.
Component IDs are unique within their parent. The example's placed paths are
`/rack`, `/rack/left` and `/rack/right`. Paths escape `~` as `~0` and `/` as `~1`.
Labels and sibling order do not change identity. Changing an ID, deleting an
instance or moving it under a different parent requires explicit reference repair.
Repeated calls and loops must supply explicit distinct IDs.

The kind names are registered language metadata. Definitions require literal IDs
and labels; call metadata evaluates in the native caller context. Calls cannot
supply child geometry or an empty child block. A marked template requires a
Component call. Ordinary modules and unassigned geometry retain native behavior.

A Part represents its finished geometry: put its cuts and unions inside the Part.
Assemblies arrange explicit Components through groups, transforms and colors.
Boolean operations, hull, Minkowski, render and projection crossing Component
boundaries are diagnosed. Anonymous geometry inside an Assembly is also diagnosed.
Share machining parameters by explicitly passing them into each Part.

## Tree and parameters

The Model tree shows placed instances and a separate Definitions group. A mixed
model also shows Other geometry; an ordinary model shows one Model entry. Picking
a mapped Part selects its exact placed path, including cut surfaces. Assembly and
definition selection comes from the tree. Source navigation uses retained native
spans. The Inspector shows actual parameters, placement transforms and target-job
status, with fit, navigation, Section creation and explicit export actions.

Eyes and Solo change inspection only. Solo temporarily overrides eye state;
clearing Solo restores it. These operations do not reevaluate the program, rebuild
meshes or change export geometry. Eye state and selection belong to each document.
Invalid or changed input disables current target actions while the last display
may remain visible.

The existing Customizer dock has Global and Model tabs. Global retains native
Customizer values and presets. Model has Selected target, Definition defaults and
All parameters modes. All parameters is grouped inspection. A selected definition
or the deliberate defaults mode displays the wider scope of default edits.

Model controls reuse native boolean, number, text and vector widgets. They show
resolved values, their default/override/expression origin, source expression and
source owner. Apply writes a literal source override or default. A missing instance
override is inserted as a named argument. Reset removes a safely delimited named
override; positional arguments cannot be reset this way.

An instance edit requires one native invocation of its source call across the
entire evaluation, including hidden and root-excluded instances. Expressions,
ambiguous bindings, unsupported Values and comment-bearing literal expressions
require source editing. A computed default can still be overridden at a unique
call. Definition-default edits preserve explicit call overrides.

Only the exact open writable source editor captured by the evaluation can be
patched. Explicitly open a library and reevaluate before editing its defaults.
No file is implicitly opened for editing or saved. Source navigation can open a
source for inspection. Transactions check the document, revision, fingerprint,
expected UTF-8 spans and semantic context again after the synchronous external-edit
hook. A rejected edit changes no source or undo history. Successful edits use one
normal undo group, ordinary preview and path rebinding; Save remains explicit.

## Targets and references

Component export defaults to Local coordinates. As placed applies its complete
model transform. Assembly Local preserves relative child placement. Definition
export evaluates the retained template's defaults, captured global configuration
and `$preview` setting. Export supports applicable native geometry formats and
their existing options. Preparation is asynchronous and cancellable. File output
is staged beside the destination and committed only while its result is current.

Semantic references use the existing design declaration contract:

```scad
@{type:Section}
cut = {
  id:"bracket-cut",
  target:{domain:"semantic", id:"/rack/left", data:{role:"instance"}},
  frame:{domain:"semantic", id:"/rack/left", data:{role:"instance"}},
  origin:[0,0,1.5], normal:[0,0,1]
};
```

Definition references use `id:"bracket", data:{role:"definition"}`. Missing,
changed, preparing and unsupported outcomes remain explicit. Section frames
require a proper rigid transform; mirrored, singular and nonuniformly scaled
frames cannot be substituted for a rigid frame.

The source branch builds on selection/inspection (03), provenance (06), design
declarations (14) and source transactions (02). It does not depend on drawing,
attachment, authoring or provider implementations. Extension APIs in
`SemanticPlacement.h` offer scoped native ownership and ordered placement passes
before publication. Generic prerequisite follow-ups and verification inputs are
retained separately from this feature's patch in its workspace run.
