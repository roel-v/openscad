# OpenSCAD agent HTTP API

The process owns one numeric-loopback listener and one full agent API at `/v1`. Enable it through
**Window > Agent Access** or the annotation panel's agent-access controls. Both control the same service
and remembered setting. Discovery records live in `OpenSCAD/AgentServer` under the platform's application
data directory. Stopping access invalidates the server identity and pending operations.

Always inspect live `capabilities`. Phase headings describe implementation history. No MCP or arbitrary-execution
endpoint exists. No dedicated client is required.

## Self-description and direct downloads

GET `/v1/info` supplies `links` to `/v1/help` (JSON catalog, fields, examples and workflow rules), `/v1/schema`
(JSON record schemas), `/v1/help/contract` (this text), and `/v1/status`. Help is compiled into the executable.
All routes except initial info require both agent headers. Discovery identifies the port; the API describes usage.
Receipts include `status_route`; queued/running responses include `poll_after_ms`. Poll with a bounded local deadline.
A timeout does not authorize a new request ID. Errors include `help_route`.

Annotation detail and successful captures include `image_route`, `png_bytes` and server-computed `png_sha256`.
GET that route with the same headers for `image/png`. Responses include Content-Length and X-Content-SHA256;
save with ordinary HTTP tools to a new local file. No base64 payload is sent in JSON.
Capture images live at `/v1/requests/{request_id}/image`: 409 pending, 404 no image, 410 expired/unknown.
Annotation images live at `/v1/annotations/{id}/image?document_id={id}` and share document/evidence lifetime.
Raw images cap at 32 MiB and count toward the shared 128 MiB outcome budget; expiry frees their bytes.

## Connection and identity

Discovery supplies `url`, `server_id`, `product`, executable and process ID. Verify it against the selected server;
never select the first record when multiple instances could be intended. Initial `GET /v1/info` requires
`X-OpenSCAD-Agent: 1`; other requests also require `X-OpenSCAD-Agent-Server: <server_id>`.
Use the exact numeric-loopback Host/port, no redirects, no Origin or Sec-Fetch headers. POST uses application/json.
These guards trust local processes; identifiers and headers are not secret authentication.

Info reports `api_version: 1`, `schema_version: 1`, live capabilities,
limits and GUI heartbeat age/responsiveness. HTTP responsiveness alone does not prove GUI responsiveness.
`GET /v1/status` returns the published GUI snapshot plus heartbeat information without triggering evaluation.

## Implemented foundation

| Method and path | Contract |
| --- | --- |
| GET `/v1/info` | Identity, version, capabilities and actual resource limits |
| GET `/v1/status` | Last published state and independent GUI heartbeat |
| GET `/v1/requests/{request_id}` | Original request outcome; 410 for expired/unknown receipts |
| POST `/v1/requests/{target_id}/cancel` | Own UUID `request_id` and `server_id`; cancel queued target only |

Cancellation returns a succeeded receipt whose result contains `cancelled` and `target_state`. False cancellation
does not imply interruption; running native code continues. The cancellation itself is idempotent under its own ID.

Jobs transition queued -> running -> succeeded/failed, or queued -> cancelled. Outcomes include `execution_started`.
A lost response requires inspecting the same server/request ID, or sending identical input under that ID.
Changing its route/body conflicts. Never retry an uncertain operation under a new ID or replacement server.

Full outcomes expire after five minutes. A bounded small receipt retains original identity, execution state and
request fingerprint until server restart; the ID cannot execute again. Expired receipts expose `terminal_state`.
Limits are 32 pending operations, 128 full outcomes, 128 MiB encoded outcome data and 10,000 receipts. At receipt
capacity, new work is rejected until deliberate restart. The queue start deadline is ten seconds; this does not
interrupt executing native code. Stop invalidates the server identity and pending work; late replies cannot attach
to its replacement. Requests/headers are limited to 256/16 KiB with 16 simultaneous connections and bounded sockets.

Errors use HTTP status plus `code`/`message`: 400 invalid request, 403 transport/consent, 404 unsupported route,
409 request conflict/stale/busy, 410 expired, 413 size, 415 media type, 429 capacity, 503 unavailable.
Terminal job errors retain their original request identity.

## Subsequent-phase inventory

### Document and source inspection (phase 2)

GET `/v1/documents` and `/v1/viewports` expose runtime IDs and separate active/displayed owners.
`source_revision` is a monotonic editor revision; `source_sha256` on a **document** hashes editor UTF-8.
`state_generation` changes with state, independently of the GUI heartbeat. Neither read evaluates or opens files.

POST `/v1/documents/{id}/source-snapshots` requires document/window/server/request IDs and explicit `origin`.
Editor origin additionally requires `expected_source_revision`; disk origin always reads the document's saved path
and fails for untitled documents. Optional `expected_source_sha256` compares normalized captured content.
No arbitrary source path is accepted. The receipt result supplies `snapshot_id` and provenance.
GET `/v1/source-snapshots/{id}` supplies the first text page; follow the returned `next_cursor` route verbatim
(`/v1/source-snapshots/{id}/pages/{n}`), until null. Pages never split UTF-16 surrogate pairs.
The snapshot's `source_sha256` hashes UTF-8 after BOM removal and CRLF/CR-to-LF normalization; `disk_sha256` hashes
the original disk bytes and is null for editor origin. Invalid UTF-8 is rejected. A disk snapshot's editor revision
is contextual, not a claim of filesystem versioning. `filesystem_atomic:false` records the nontransactional read;
size/path/mtime changes observed during reading cause failure, but same-metadata external replacement is not detectable.

At most eight source snapshots and an estimated 32 MiB of retained text/pages are kept, with oldest-first eviction
and a five-minute lifetime. Each root is limited to 4 MiB. Closure, replacement or Stop invalidates handles. Missing
snapshot/page reads return 410; recapture is an explicit new operation, never an automatic fallback to current text.

GET `/v1/documents/{id}/attempts`, `/results`, `/diagnostics` returns bounded observed history (64/32/128 records).
Attempts identify actual editor-derived evaluation inputs. Only successful publication creates a result record;
a failure adds an attempt while the viewport continues to report its actual renderer/owner independently.
Diagnostics are typed native messages, with context captured before worker-to-GUI delivery and unchanged console
delivery. History is explicitly incomplete; there is no reconstruction from the console. Unverified native source
coordinates are separate from `location:null`. Dependency reads initially report unavailable provenance rather
than infer dependencies from editor text; parse/evaluation phases enrich these records.

Later phase implementations extend this document with exact response fields and capability names. Their fixed
route layout is below; unimplemented routes return unsupported and are absent from capability discovery.

- GET `/v1/documents`, `/v1/viewports`.
- POST `/v1/documents/{document_id}/source-snapshots`: explicit `origin: disk|editor`, expected revision/hash.
- GET `/v1/source-snapshots/{snapshot_id}`: immutable bounded text/provenance, paged when needed.
- POST `/v1/documents/{document_id}/parse`: retained `snapshot_id`, never substituted current editor text.
- GET `/v1/documents/{document_id}/dependencies`, `/attempts`, `/results`, `/diagnostics` (each fully document-nested).
- GET `/v1/trees/{tree_id}`, `/v1/trees/{tree_id}/nodes/{node_id}` and `/children` under that node.
- Optional GET `/v1/documents/{document_id}/parameters` and `/geometry-summary` under that document.
- GET `/v1/annotations?document_id=...` and `/v1/annotations/{id}?document_id=...`.
- POST `/v1/viewports/{viewport_id}/view`, `/v1/capture`, `/v1/annotations/{id}/restore`.
- Later navigation adapter only: GET/POST `/v1/documents/{document_id}/saved-views` and record-specific actions.

Every operation carries its own `request_id`, correct `server_id` and explicit target fields. Document/window/viewport
IDs are runtime identities, not paths. Source operations do not require a rendered model. Camera/capture operations
require expected result and view revisions. Tree IDs scope node IDs to one snapshot; never reuse them after eviction.
Disk and editor provenance remain distinct. Source inspection never writes files, saves, switches tabs or evaluates.

The source/node/outcome wire records and bounded scalar/property encodings are specified in
[schemas.json](schemas.json). The bundled agent client supports inspection, annotations and camera workflows.

## Parse and syntax trees (phase 3)

POST `/v1/documents/{id}/parse` requires document/window/server/request IDs and a retained `snapshot_id`.
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

GET `/v1/trees/{id}`, `/nodes/{node_id}` and `/nodes/{node_id}/children` under it reads detached immutable records.
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
become successful results. Attempts use `attempt_id`; successful results use `result_id`. A tree's `kind:evaluated`
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

GET `/v1/documents/{id}/parameters` exposes typed current Customizer `values`, `origin:current_customizer`, contextual
`source_revision`, `defined_from_source_revision`, and `stale_source`. Definitions may predate an edit. Metadata is
unavailable; a response exceeding 64 KiB reports unavailable instead. This read never sets parameters or evaluates.

## Evidence, camera and capture (phase 5)

GET `/v1/annotations?document_id={id}&q={text}` reads accepted native evidence, including unsaved annotations.
The optional case-insensitive comment search is data. Follow `next_cursor` for list pages (100 records/768 KiB).
GET `/v1/annotations/{annotation_id}?document_id={id}` returns immutable original metadata and `image_route`.
Evidence reads remain available during drawing/compilation; no historical image is rendered again.

POST `/v1/viewports/{id}/view`, `/v1/capture`, and `/v1/annotations/{id}/restore` require request/server/document/
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
Queued/running/terminal receipts retain their identity, including failure. All camera/capture requests require
the displayed result ID and view revision; a numeric generation alone is not a valid result guard.

## Delivery limits and state details

`api_version:1` and `schema_version:1` identify the agent API. Viewports expose `busy_reasons`, `capture_available`,
and the actual reload setting. Native attempts report elapsed `duration_ms`. Global messages, separate from any
document attempt, are available at GET `/v1/windows/{window_id}/diagnostics`.
Each document/global history has an additional 512-KiB encoded bound, dropping oldest records. Captured result
Customizer context exceeding 64 KiB is null; model dependency manifests cap at 64 KiB and remain explicitly incomplete.
JSON responses cap at 1 MiB; raw PNG downloads cap at 32 MiB. Excessive output returns `response_limit`.
These bounds supplement record counts and shared source/tree/job retention. No read recomputes geometry.

## Native navigation adapter (phase 7)

When `saved_views.read`/`saved_views.write` are advertised, GET `/v1/documents/{id}/saved-views` reads the native library
of the displayed document. Another active editor does not retarget it. Non-displayed documents report unavailable.
The response includes document/window/viewport IDs, opaque `library_revision`, dirty/problem state and ordered
`views:[{id,name,camera}]`. Cameras use the same public SCAD conventions as `/view`. Follow `next_cursor` for pages of
100 entries; cursors include the library revision and expire with 410 after native changes. No read opens a companion.

POST the list route to add the displayed full camera under the native default name; the result returns `added_id`.
POST `/v1/documents/{doc}/saved-views/{id}/rename` with `name`, or `/remove`, or `/recall`. Every operation requires
request/server/document/window/viewport and `expected_library_revision`. Add and recall additionally require expected
result/view guards. Admission and dispatch reject changed ownership/library state; identical receipt replay never
duplicates additions or deletions. Results return library metadata, `views_route` and `viewport` state.

Add freezes a native transition at its displayed camera. Recall restores pose/distance/FOV/projection immediately
through the native operation boundary, with one final notification, no fit, guide or viewport-size restoration.
Core agent camera actions likewise use immediate native application and cancel queued transitions; UI animation
preferences remain unchanged. Native animation/script-camera consumers may still reevaluate afterward.
Rename/remove never move the camera. Names follow native trim/256-UTF-16-unit rules; a blank name leaves the old name.
The native library caps at 1,000 entries. Native load/conflict problems reject mutations; no alternate store is created.

All changes remain in native memory until ordinary Save/Save As/copy/all; source reload preserves them. No HTTP save
endpoint exists. Metadata-only Save preserves source timestamps; copy retains original dirty state. The native
feature owns persistence and history policy. IDs/revisions are scoped to the live owning document, not file paths.

With this adapter, all fresh agent captures use native ModelOnly readback. Its scope guard excludes axes,
scale markers, crosshairs, statistics, navigation controls, palette and historical marks, retaining model surfaces
and edges and restoring live preferences on every exit. `capture_policy:model_only_without_guides_statistics_or_controls`
describes this integrated policy. Original annotation images stay immutable. Capture cancels remaining native camera
transition callbacks before binding its frame, and never starts another animation controller.
