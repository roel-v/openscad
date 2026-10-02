# OpenSCAD agent server

Enable **Window > Agent Access** in the GUI. Broad access is remembered; Stop invalidates that service session.
Older annotation-only consent exposes only `/v1`. Both versions share one numeric-loopback HTTP listener.
No MCP, remote listener, arbitrary execution, source writes or forced evaluation is provided.

Copy this entire folder anywhere, including into an agent's skills directory. Python 3.9+ needs only its standard
library. No build tree, repository imports or third-party packages are required. [SKILL.md](SKILL.md) teaches the
file-edit/reload/inspect loop; [HTTP.md](HTTP.md) specifies the contract and [schemas.json](schemas.json) its records.

```text
python scripts/client.py instances
python scripts/client.py --connection connection.json status
python scripts/client.py --connection connection.json source --document DOC --window WIN --origin disk
python scripts/client.py --connection connection.json snapshot SNAPSHOT
python scripts/client.py --connection connection.json parse --document DOC --window WIN --snapshot SNAPSHOT
python scripts/client.py --connection connection.json tree TREE
python scripts/client.py --connection connection.json node TREE NODE
python scripts/client.py --connection connection.json children TREE NODE
python scripts/client.py --connection connection.json get /v1/trees/TREE/nodes/NODE/children/pages/1
python scripts/client.py --connection connection.json diagnostics --document DOC
python scripts/client.py --connection connection.json annotations --document DOC --query hole
python scripts/client.py --connection connection.json annotation ANNOTATION --document DOC --output original.png
python scripts/client.py --connection connection.json view --document DOC --window WIN --viewport VIEWPORT --expected-result RESULT --expected-view REV --operation standard --view top
python scripts/client.py --connection connection.json capture --document DOC --window WIN --viewport VIEWPORT --expected-result RESULT --expected-view REV --output current.png
python scripts/client.py --connection connection.json outcome REQUEST
```

Replace uppercase IDs with values from actual responses. Use returned `next_cursor` verbatim with `get`; example
page numbers are illustrative. Every non-discovery command requires explicit `--connection`; stale identity fails.
Copy connection info into a JSON file or use a live discovery file listed by `instances`. Source commands require
explicit disk/editor origin; editor needs `--expected-revision`. Optional `--dependency` selects a known parse handle.
Read command `--help` for projection, fit, full camera, restore, cancellation and diagnostic/history commands.

Operations return receipts containing `result`. A succeeded parse operation can contain `result.success:false`.
Read `outcome` to recover an uncertain request; the client never retries or follows redirects. `--request-id` permits
an intentional identical replay, and `post ROUTE --body body.json` supports exact saved request bodies. Errors and
request IDs go to stderr, JSON to stdout. Exit 0 means the command completed; exit 1 means error/uncertainty, and
argument syntax errors exit 2. A successful `outcome` read may still describe a failed/running operation.

PNG data is suppressed from stdout. `--output` validates its SHA-256, chunk CRCs and dimensions before exclusively
creating the requested file; existing files are never overwritten. No screenshots are written without an output path.

Current optional inspection exposes Customizer values with definition provenance, but no complete parameter metadata
or geometry metrics. Native SCAD coordinates have no assumed physical units. Saved views are capability-gated and
owned by the native navigation feature. The core service works independently of that integration.

The listener trusts local processes. Headers and browser-request rejection reduce accidental browser access; they
are not authentication against local code. Do not expose it through a network proxy. Stop disables new requests;
already running noninterruptible native work can finish, but cannot publish into another session.

Run client regressions with `python -m unittest discover -s tests`. Native Qt tests cover GUI identity, parser/model
lifetimes, camera locks and real captures. Platform/build evidence belongs to the distributor; these files do not
claim clean-machine or non-Windows validation.

When navigation capabilities are advertised, `saved-views --document DOC` lists its native entries. Use `saved-add`,
`saved-rename ID --name NAME`, `saved-remove ID`, or `saved-recall ID`, each with explicit document/window/viewport
and `--expected-library REV`. Add/recall also require `--expected-result` and `--expected-view`. These commands never
save files. Read their help for complete arguments. Clean integrated capture excludes guides and navigation controls;
the viewport retains its live preferences. Core-only builds do not advertise saved views.
