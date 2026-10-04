# Native design objects

`@{type: Section}` and `@{type: View}` mark file-scope assignments. The native evaluator
reads each effective assignment's already evaluated value once. Includes retain the root
lexical environment; `use` exports helpers, not top-level design objects. Invalid types,
marked reassignment, local built-in declarations and duplicate explicit IDs produce
diagnostics. A duplicate invalidates both declarations, with no preferred winner.

Every definition has a nonempty case-sensitive `id` and an optional `label`. Labels and
variable names are presentation, not identity. Geometry evaluation does not depend on these
records. Published records own detached immutable data and belong to one evaluated root.

Sections require finite three-vectors `origin` and nonzero `normal`. Optional `up` must
be nonparallel; its default is world Z, then world Y when necessary. `adjust` holds
`translation: [x,y,z]`, normalized `rotation: [x,y,z,w]`, and scalar `offset`, with zero,
identity and zero defaults. Rotation precedes offset along the rotated normal. Translation
and offset never replace a parametric base origin such as `[0,0,height/2]`.

Views use `projection: "orthographic"` or `"perspective"`. A fit view requires `direction`
and `up`, with optional nonnegative `margin` (default 0.05). A fixed view instead requires
`camera: {rotation:[x,y,z], translation:[x,y,z], distance:d, fov:f}` in OpenSCAD's
`$vpr`/`$vpt` conventions. Distance is positive; FOV lies strictly between 0 and 180.
Fit and fixed fields cannot be mixed. Fit uses current target bounds and is unavailable
for an empty target.

References use `{domain: "world"|"document"|"design"|"geometric", id: "...", ...}`.
Design references resolve only exact IDs. Missing, changed, unsupported and invalid are
distinct results. Geometric references retain source/configuration fingerprints; no nearest
match is substituted. Frames must be finite rigid transforms. Required dependencies are
planned before persistence, ordered before their consumers, and rejected on missing IDs,
cycles, variable collisions or a sourced dependency outside the current include closure.

`DesignDocument` is owned by a root editor. `createTransient`, `beginEdit`, `updateDraft`,
`cancelDraft`, `commitDraft`, `promote` and `remove` maintain one shared Section/View record
model. A stamp identifies the root, evaluation and captured inputs. A gesture previews a
draft; committing a source edit goes through `DesignEdits::prepare` and the existing
`SourceEditing` transaction. Promotion uses an explicitly supplied open destination and
never creates an include or writes a closed file. `saveSequentially` reports outcomes per
editor, including preceding successes when a later save is canceled or fails.

The bounded `DesignSourceMap` verifies the captured annotation and assignment, then edits
only selected literal fields. It preserves UTF-8, comments, LF/CRLF, comma style and other
expressions. Helper/computed/duplicate-key definitions remain evaluable but source-only
for graphical field editing. An expression in `adjust.offset` disables numerical edits of
that field. Explicit deletion requires ownership of the complete declaration; surrounding
comments remain outside it.

`EditorUndoRegistry` owns opaque container actions for both annotations and design
lifecycle changes. The optional transaction participant carries an owner lifetime and
undo/redo callbacks, not source buffers. Validated text and lifecycle changes occupy one
native undo group. Initial lifecycle callbacks run after that group closes. Clearing
history, owner destruction and editor closure release registrations and callbacks.

Extension kinds register supported hosts/scopes and a decoder with `registerKind`.
Document-registry and consumer destinations are explicit. Consumer callbacks receive a
borrowed native declaration context only during evaluation. Their opaque owner-qualified
keys never enter the global ID map. A root's RAII lifecycle adapter supplies
`resolveConsumerRecord`, `prepareInsert`, `prepareEdit` and `locatePromotedRecord`.
Promotion plans required Section/View dependencies, preserves draft snapshots in undo,
and rebinds through that adapter after reevaluation. Destroying its owner unregisters it.

Personal `.views.json` entries retain their personal inspection backing and use the same
immutable View definition. Ordinary save preserves the existing companion policy and
conflict handling. Source View definitions are never duplicated into that file. Explicit
promotion retains the personal ID and removes personal membership only after a successful
source transaction; undo restores it. The inspector labels Source, Transient and Personal
inspection entries and provides Apply, Promote, Rename and Delete. No legacy importer or
automatic companion rewrite/deletion is introduced.
