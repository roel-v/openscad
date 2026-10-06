# Captured source provenance

`SourceProvenance::Result` belongs to one `Selection::EvaluatedScene` and its evaluation inputs.
It contains copied syntax and invocation records, with no editor or AST pointers. The published
syntax table is sealed, including its line-start cache, and can be retained after the editor closes.
The GUI service still rejects that retained result for navigation or edits once its binding is stale.

`Span` uses exact captured UTF-8 bytes and an exclusive end: `[begin,end)`. Locations use one-based
lines and byte columns. LF starts a new line; CR consumes a byte. BOMs, multibyte characters,
comments, parentheses and the original expression spelling remain in the captured text. Whole
statements include their semicolon. Argument expression ranges exclude the name, equals sign
and separators. `SyntaxTable::original` checks file identity, fingerprint, bounds and UTF-8 boundaries.
An include expansion crossing files can have a known call header and arguments while
`Site::wholeComplete` is false; its whole range cannot be edited.

`SyntaxKey` ordinals and `Instance::id` are scoped to this input set and evaluation. A repeated
call shares syntax but receives a separate invocation path. A user-module invocation has distinct
`call` and `definition` records. Its descendants retain their more specific emitting invocations.
Offsets, node indices, invocation IDs and loop ordinals do not identify a part across recompilation.

`Result::forNode` describes logical evaluated nodes. `withPreview` publishes a new result with
the actual CSG leaf transforms and subtraction polarity; normalization preserves their node indices.
Geometry caches keep geometry only, so two instances can share a mesh while having different
owners and transforms. Singular transforms remain navigable but have no invertible local frame.
Repeated leaf indices with conflicting transforms or polarity are explicitly ambiguous.

Preview primitives identify their emitting invocation. A subtraction boundary identifies its cutter
invocation, with the containing boolean in its parent path. This does not identify the whole final
part. Render, hull, Minkowski, projection and import results can identify their producing call;
inner face ownership is unavailable. Final merged faces expose candidate producers, with
`Multiple` or `Unavailable` confidence when ownership is ambiguous. No candidate receives edit priority.

The GUI API is `gui/selection/ProvenanceService.h`. `forWindow` retrieves the window-owned service.
`resolve(hit)` validates the complete selection Stamp and captured inputs, and checks a preview
hit's transform against the source frame. `navigationCandidates` includes each known producer's
invocation ancestors. The Inspector lists these for explicit call or definition navigation.
Library navigation uses normal tab opening and rechecks the exact target and bytes after activation.
It never writes a library or assigns its source span to whichever tab happens to be active.

`editableSpan(resolved, instance, span, replacement)` only prepares an `EditableTarget`. It requires
a current unique producer, an explicitly selected invocation or ancestor, a range in that invocation's
recorded call/definition syntax, and the exact captured editor identity, revision and fingerprint.
Read-only, closed and disk-only sources are rejected. Opening a disk-only library for navigation
does not make it editable until another evaluation captures that editor. `validateTarget` rechecks
the evaluation binding and feature 02 transaction preconditions. Consumers must validate immediately
before calling `SourceEditing::apply` on the captured `QPointer`; 02 rechecks expected bytes and
synchronous editor observers before applying one Undo group. This API does not choose expression rewrites.

The core tests use `[source-provenance]`. The native `TestProvenance` class is selected with
`OPENSCAD_PROVENANCE_TESTS_ONLY`, and also runs in the full GUI suite. They cover exact source ranges,
dirty dependencies, repeated calls/loops/children, shared geometry, normalized transforms, singular
frames, exceptions, actual preview/final picks, source Undo, definition navigation, stale recompilation,
parse failure, tab races and closed editors. There is no persistent topology naming or semantic Part type.
