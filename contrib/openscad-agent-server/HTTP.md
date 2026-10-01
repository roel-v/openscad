# OpenSCAD agent HTTP API

The process owns one numeric-loopback listener. Existing annotation clients retain `/v1`, their headers and the
`AgentAnnotations/OpenSCAD` discovery directory. `/v2` requires explicitly enabled broad access through
**Window > Agent Access**. Legacy annotation consent alone does not enable `/v2`. Stop affects both versions.

This contract is developed in phases. Always inspect live `capabilities`; an operation listed in the later inventory
is not available merely because a future phase is documented. No MCP or arbitrary-execution endpoint exists.

## Connection and identity

Discovery supplies `url`, `server_id`, `product`, executable and process ID. Verify it against the selected server;
never select the first record when multiple instances could be intended. Initial `GET /v2/info` requires
`X-OpenSCAD-Agent: 1`; other requests also require `X-OpenSCAD-Agent-Server: <server_id>`.
Use the exact numeric-loopback Host/port, no redirects, no Origin or Sec-Fetch headers. POST uses application/json.
These guards trust local processes; identifiers and headers are not secret authentication.

Info retains legacy fields and reports `api_version: 2`, `agent_api_versions`, `access_mode`, live capabilities,
limits and GUI heartbeat age/responsiveness. HTTP responsiveness alone does not prove GUI responsiveness.
`GET /v2/status` returns the published GUI snapshot plus heartbeat information without triggering evaluation.

## Implemented foundation

| Method and path | Contract |
| --- | --- |
| GET `/v2/info` | Identity, version, capabilities and actual resource limits |
| GET `/v2/status` | Last published state and independent GUI heartbeat |
| GET `/v2/requests/{request_id}` | Original request outcome; 410 for expired/unknown receipts |
| POST `/v2/requests/{target_id}/cancel` | Own UUID `request_id` and `server_id`; cancel queued target only |

Cancellation returns a succeeded receipt whose result contains `cancelled` and `target_state`. False cancellation
does not imply interruption; running native code continues. The cancellation itself is idempotent under its own ID.

Jobs transition queued -> running -> succeeded/failed, or queued -> cancelled. Outcomes include `execution_started`.
For compatibility, legacy `/v1` reports running jobs as queued until terminal; `/v2` exposes the running state.
A lost response requires inspecting the same server/request ID, or sending identical input under that ID.
Changing its route/body conflicts. Never retry an uncertain operation under a new ID or replacement server.

Full outcomes expire after five minutes. A bounded small receipt retains original identity, execution state and
request fingerprint until server restart; the ID cannot execute again. Expired receipts expose `terminal_state`.
Limits are 32 pending operations, 128 full outcomes, 128 MiB encoded outcome data and 10,000 receipts. At receipt
capacity, new work is rejected until deliberate restart. The queue start deadline is ten seconds; this does not
interrupt executing native code. Stop invalidates the server identity and pending work; late replies cannot attach
to its replacement. Requests/headers are limited to 256/16 KiB with 16 simultaneous connections and bounded sockets.

Errors use HTTP status plus `code`/`message`: 400 invalid request, 403 transport/consent, 404 unsupported route,
409 request conflict/stale/busy, 410 expired, 413 size, 415 media type, 429 capacity, 503 unavailable. Legacy `/v1`
keeps its established error shape. Terminal job errors retain their original request identity.

## Subsequent-phase inventory

### Document and source inspection (phase 2)

GET `/v2/documents` and `/v2/viewports` expose runtime IDs and separate active/displayed owners.
`source_revision` is a monotonic editor revision; `source_sha256` on a **document** hashes editor UTF-8.
`state_generation` changes with state, independently of the GUI heartbeat. Neither read evaluates or opens files.

POST `/v2/documents/{id}/source-snapshots` requires document/window/server/request IDs and explicit `origin`.
Editor origin additionally requires `expected_source_revision`; disk origin always reads the document's saved path
and fails for untitled documents. Optional `expected_source_sha256` compares normalized captured content.
No arbitrary source path is accepted. The receipt result supplies `snapshot_id` and provenance.
GET `/v2/source-snapshots/{id}` supplies the first text page; follow the returned `next_cursor` route verbatim
(`/v2/source-snapshots/{id}/pages/{n}`), until null. Pages never split UTF-16 surrogate pairs.
The snapshot's `source_sha256` hashes UTF-8 after BOM removal and CRLF/CR-to-LF normalization; `disk_sha256` hashes
the original disk bytes and is null for editor origin. Invalid UTF-8 is rejected. A disk snapshot's editor revision
is contextual, not a claim of filesystem versioning. `filesystem_atomic:false` records the nontransactional read;
size/path/mtime changes observed during reading cause failure, but same-metadata external replacement is not detectable.

At most eight source snapshots and an estimated 32 MiB of retained text/pages are kept, with oldest-first eviction
and a five-minute lifetime. Each root is limited to 4 MiB. Closure, replacement or Stop invalidates handles. Missing
snapshot/page reads return 410; recapture is an explicit new operation, never an automatic fallback to current text.

GET `/v2/documents/{id}/attempts`, `/results`, `/diagnostics` returns bounded observed history (64/32/128 records).
Attempts identify actual editor-derived evaluation inputs. Only successful publication creates a result record;
a failure adds an attempt while the viewport continues to report its actual renderer/owner independently.
Diagnostics are typed native messages, with context captured before worker-to-GUI delivery and unchanged console
delivery. History is explicitly incomplete; there is no reconstruction from the console. Unverified native source
coordinates are separate from `location:null`. Dependency reads initially report unavailable provenance rather
than infer dependencies from editor text; parse/evaluation phases enrich these records.

Later phase implementations extend this document with exact response fields and capability names. Their fixed
route layout is below; unimplemented routes return unsupported and are absent from capability discovery.

- GET `/v2/documents`, `/v2/viewports`.
- POST `/v2/documents/{document_id}/source-snapshots`: explicit `origin: disk|editor`, expected revision/hash.
- GET `/v2/source-snapshots/{snapshot_id}`: immutable bounded text/provenance, paged when needed.
- POST `/v2/documents/{document_id}/parse`: retained `snapshot_id`, never substituted current editor text.
- GET `/v2/documents/{document_id}/dependencies`, `/attempts`, `/results`, `/diagnostics` (each fully document-nested).
- GET `/v2/trees/{tree_id}`, `/v2/trees/{tree_id}/nodes/{node_id}` and `/children` under that node.
- Optional GET `/v2/documents/{document_id}/parameters` and `/geometry-summary` under that document.
- GET `/v2/annotations?document_id=...` and `/v2/annotations/{id}?document_id=...`.
- POST `/v2/viewports/{viewport_id}/view`, `/v2/capture`, `/v2/annotations/{id}/restore`.
- Later navigation adapter only: GET/POST `/v2/documents/{document_id}/saved-views` and record-specific actions.

Every operation carries its own `request_id`, correct `server_id` and explicit target fields. Document/window/viewport
IDs are runtime identities, not paths. Source operations do not require a rendered model. Camera/capture operations
require expected result and view revisions. Tree IDs scope node IDs to one snapshot; never reuse them after eviction.
Disk and editor provenance remain distinct. Source inspection never writes files, saves, switches tabs or evaluates.

The source/node/outcome wire records are specified in [schemas.json](schemas.json); subsequent phases implement
their adapters and bounded scalar/property encodings. Until then,
the legacy evidence client continues to provide the already implemented annotations/camera workflow on `/v1`.

## Parse and syntax trees (phase 3)

POST `/v2/documents/{id}/parse` requires document/window/server/request IDs and a retained `snapshot_id`.
It holds the native compilation gate, parses those immutable bytes, and never evaluates modules or imports.
The operation receipt's `succeeded` means the parse operation ran; its result's `success` reports whether syntax
parsing succeeded. Invalid syntax returns its own diagnostics and no tree. `/parse-result` under the document
returns the latest parse outcome; `/diagnostics` also includes a separately identified `latest_parse` array.
`editor_advanced` records whether the editor revision has moved since capture. Historical input is never replaced.

The result supplies `parse_id`, `tree_id` when successful, dependencies, source provenance and duration. Include
content is bounded and cached during the operation; use libraries are parsed once each without ModuleCache or
dependency-command side effects. Relative/library resolution uses the native lookup rules. Missing/recursive
includes remain reported warnings; `dependencies_complete:false` prevents treating partial knowledge as proof.
Dynamic imports are syntax-only records with unresolved paths and linked node IDs. No imported geometry is read.
Known include/use entries carry opaque `dependency_id` values: a subsequent source-snapshot request can explicitly
select that handle with `origin:disk`. Handles belong to the document's latest parse, and never accept a path override.
Dependency hashes refer to bytes actually read; cross-file reads are not filesystem transactions.

GET `/v2/trees/{id}`, `/nodes/{node_id}` and `/nodes/{node_id}/children` under it reads detached immutable records.
Follow `next_cursor` routes verbatim to traverse child pages (up to 500 edges). IDs are local to their tree; expired
trees or invalid nodes/pages return 410. Syntax preserves native lexical statement order, including overwritten
assignments and disabled calls. Named child roles expose expressions, arguments, parameters and control branches.
Literal values are syntax literals, never inferred runtime values; undefined/non-finite values are typed objects.
Source spans use one-based UTF-8 byte columns with exclusive end, in the bytes parsed for that file; a module
definition's native span may cover its declaration header. Missing/unverified locations are null.

Inspection caps: root/file 4 MiB, total dependency bytes 32 MiB, 256 dependency entries, 64 nested include files,
8,192 lexical tokens and 256 delimiter nesting. A cooperative two-second parsing deadline is checked at token/file
boundaries; it cannot interrupt a blocked filesystem call. Tree traversal caps 100,000 nodes/depth 256 and reports
incomplete nodes explicitly. Strings cap at 4,096 UTF-16 units. At most eight trees with five-minute expiry and
an estimated 32 MiB retention budget coexist; source pages use their separate 32 MiB budget. Every tree response
is capped at 1 MiB. Expiry is enforced on the HTTP worker even while the GUI is busy. No native AST pointers cross
that boundary. Closing/replacing a document or restarting the service invalidates its handles.

## Evaluated models and optional fields (phase 4)

Successful native evaluation publishes a qualified `result_id`, and a `tree_id` when inspection fits its budgets.
The viewport's `displayed_result_id` identifies an installed or successfully empty result; failed attempts do not
become successful results. Legacy `result.id` remains its original numeric generation. A tree's `kind:evaluated`
and `representation:instantiated_nodes` distinguish it from syntax and from normalized renderer CSG.
`root_ids` describe the absolute hierarchy; `effective_root_ids` identify native `!` selection. Children retain native
order. Expanded calls may share source locations; node IDs are unique only within that tree. Native mappings are
copied at successful publication, before source/module lifetimes end. Reads never touch native node pointers.

| Native kind | Retained typed attributes |
| --- | --- |
| cube, sphere, cylinder, square, circle | dimensions/radii, centering where native, sphere style |
| transform, color | row-major 4x4 matrix, RGBA |
| linear/rotate extrusion | height/scale/twist/centering/slices/segments; angle/start/convexity |
| projection, offset | cut/convexity; delta/chamfer |
| polygon, polyhedron | array counts; arrays explicitly omitted |
| import | resolved path, scale, DPI, center |
| Boolean/group/intersection | native kind and ordered children |
| all mapped nodes | available source span, root/highlight/background modifiers |

`property_adapter_available` describes the table above; `properties_complete:false` explicitly avoids claiming every
native field is exported (for example tessellation settings and mesh arrays are omitted). Unknown node kinds remain
traversable. Non-finite numeric values are tagged objects. No bounds, facets, triangle counts, volume, validity or
geometry-summary capability is advertised: obtaining these consistently would require additional geometry work.

Model admission caps at 12,000 nodes/depth 256, with incomplete children explicitly flagged. The shared tree retention
budget may reject a larger serialization, leaving `tree_unavailable_reason` on a successful native result. Inspection
failure never makes native compilation fail. Detached result trees are recorded even while access is disabled, so
enabling access can inspect an existing result; they share the same eight-tree/32-MiB/five-minute bounds. Stop clears
retained handles. Without another native evaluation, restarting access may expose a result whose tree has expired.
Dependencies observed by native compilation are result-scoped include/use/import records; bytes/hashes remain unknown
when not captured. They do not replace separately identified parse-only dependency manifests.

GET `/v2/documents/{id}/parameters` exposes typed current Customizer `values`, `origin:current_customizer`, contextual
`source_revision`, `defined_from_source_revision`, and `stale_source`. Definitions may predate an edit. Metadata is
unavailable; a response exceeding 64 KiB reports unavailable instead. This read never sets parameters or evaluates.

## Evidence, camera and capture (phase 5)

GET `/v2/annotations?document_id={id}&q={text}` reads accepted native evidence, including unsaved annotations.
The optional case-insensitive comment search is data. Follow `next_cursor` for list pages (100 records/768 KiB).
GET `/v2/annotations/{annotation_id}?document_id={id}` returns immutable original metadata and `png_base64`.
Evidence reads remain available during drawing/compilation; no historical image is rendered again.

POST `/v2/viewports/{id}/view`, `/v2/capture`, and `/v2/annotations/{id}/restore` require request/server/document/
window/viewport IDs plus nonempty `expected_result_id` (the viewport's `displayed_result_id`) and
`expected_view_revision`. Identical retries return the original receipt before checking current preconditions.
New operations validate cached admission state and live dispatch state; stale/busy admission returns 409, while
a change after admission produces a failed receipt. Do not refresh preconditions and silently repeat an operation.
View revisions are monotonic observations of native camera, logical dimensions and DPR; they are not source revisions.

`/view` accepts exactly one operation: `set` (default) with full `camera`, `standard` with `view` equal to top/bottom/
left/right/front/back/diagonal, `projection` with orthographic/perspective, or `fit`. Full cameras use native public
`translation:[x,y,z]`, `rotation:[x,y,z]` degrees, positive `distance`, `fov` in (0,180), and `projection`.
Vectors must be finite and bounded by 1e12; distance is bounded by 1e12. Native operations notify camera consumers;
Animate/script-camera behavior may subsequently replace the displayed result. Read fresh state before capture.
Restore applies only the annotation's historical camera to the current geometry; it never loads old source or pixels.

Capture binds owner/result/camera/revision/size/DPR and waits for a post-paint frame-ready notification. Changes before
readback fail the receipt. It copies the frame on the GUI thread, then encodes/hashes the detached image on a worker.
Timeout (four seconds), Stop and destruction invalidate callbacks. Minimized, hidden or modal targets are rejected.
PNG metadata includes width/height in physical pixels, DPR, camera, `displayed_result_id`, `view_revision`, SHA-256
and `capture_policy:native_renderer_without_annotation_controls_or_historical_marks`. Core renderer guides remain
as configured; navigation integration later supplies helper-free capture. No fixed sleep proves frame freshness.
Queued/running/terminal receipts retain their identity, including failure. `/v1` continues accepting its numeric
result guard without requiring a view revision; its unchanged client remains supported on the same listener.
