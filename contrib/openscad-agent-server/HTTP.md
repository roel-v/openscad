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
