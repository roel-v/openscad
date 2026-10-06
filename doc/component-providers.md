# Component providers

SCADMaxx ships declarative catalogs for four primitives, a BOSL2 threaded-hole mask and three BOLTS hardware families.
Catalogs generate ordinary SCAD source. Libraries are installed separately and must match an exact tested profile.
The commands below do not edit or evaluate the target document, download libraries, or open a window.

```text
scadmaxx.com --provider-list
scadmaxx.com --provider-check catalog.json --provider-source project/model.scad
scadmaxx.com --provider-emit scadmaxx.builtin:cube --provider-values values.json
scadmaxx.com --provider-list --provider-catalog extra/catalog.json
```

Use exactly one list/check/emit operation. Provider flags cannot be mixed with ordinary model/export/GUI flags.
`provider-source` supplies a document path or directory; its absence uses the current directory and reports `cwd`.
`provider-values` is a UTF-8 JSON object of typed scalars, for example `{"width":10,"depth":20,"height":30}`.
Defaults apply to omitted parameters. String-to-number conversion, source expressions and implicit unit conversion
are unsupported. Emission returns JSON containing owned recipe data, a contact frame, imports/helpers, binding proof
and `standaloneSource`. Save that SCAD string explicitly to use it independently of the catalog.

List exits 0 even when optional libraries are unavailable. Check exits 0 when the selected catalog and its dependencies
match, otherwise 1. Emit exits 1 with diagnostics and no fragment for an unavailable item or invalid values.
Usage errors exit 2. Output is UTF-8 JSON; native process diagnostics use stderr.

Catalog discovery reads direct `*.json` files in the application `providers` directory, the user configuration
directory's `providers`, and the source directory's `.scadmaxx/providers`. It does not search parent directories.
Additional paths can be supplied by repeated `provider-catalog` flags or `provider-sources.json` in user configuration.
Canonical paths are sorted and read once. Different catalogs claiming one provider ID disable every contender.
Invalid catalogs leave unrelated providers usable. Schemas/examples in subdirectories are not loaded as catalogs.

```json
{
  "schemaVersion": 1,
  "catalogPaths": ["my-catalogs"],
  "libraryBindings": [
    {"libraryId":"bosl2","root":"D:/libraries/BOSL2","importPrefix":"BOSL2"},
    {"libraryId":"bolts","root":"D:/libraries/BOLTS","importPrefix":"BOLTS"}
  ]
}
```

Configuration paths may be absolute or relative to the configuration file's directory. A root binding does not change
OpenSCAD search paths: imports must still resolve there using document-relative precedence and `OPENSCADPATH`, user
libraries, then application libraries. An empty prefix explicitly supports a flat library layout. There is no alternate
spelling search or selection of a later compatible installation after an incompatible local shadow.

Schema 1 is described by `providers/schemas/provider-v1.schema.json` and the authored `examples/providers` catalog.
Provider/item/library IDs use lowercase letter-led dotted segments. Profile IDs also allow numeric version segments,
such as `bosl2-2.0.766-e173fa0`. Provider versions are exact three-integer labels, not compatibility ranges.
The native loader additionally verifies references, helper cycles/signatures, finite arithmetic/default frames,
closure reachability, duplicate JSON keys and resource limits. Unknown fields are errors.

Recipes contain bounded expression/statement trees, never raw source templates or executable plugins. Parameters are
number/integer/bool/string/enum. Literal identifiers, named argument order, scope and import paths are preserved.
Callers supply unique concrete helper names. BOLTS imports stay inside their helper definitions; BOSL2 imports stay
at file scope. Geometry/component insertion role is independent of additive/subtractive operation; components must
be additive. A contact frame's direction is explicit, without an operation-dependent orientation flip.

Library profiles hash every file in the imported closure. Normalize UTF-8 by stripping one initial BOM and converting
CRLF/CR to LF, with no other changes. SHA-256 each normalized file. Sort relative paths by UTF-8 bytes and SHA-256 the
concatenation of `path + NUL + lowercase hash + LF`. Frozen include/use edges must reach exactly this file set and
resolve to the recorded canonical targets after all bytes match. Symlink escapes, changed files and missing captured
inputs disable the entry. Extra unreferenced files are allowed. Version strings and Git HEAD cannot authorize a match.

Mismatch diagnostics report expected profiles, the effective root, detected marker/hashes and first differing file.
To support a changed wrapper/library, its author must validate the actual source, dependency trace and geometry, then
explicitly revise the catalog version and fingerprint. Diagnostic commands never change a catalog automatically.

Public APIs are in `src/core/providers`. `discoverProviders` produces explicit catalog sources and library bindings.
`ProviderRegistry::load/query/bind/generate` use owned immutable snapshots and value types. Resolution/read callbacks
can supply captured dirty buffers without disk fallback. `revalidate(binding, context, currentSnapshot)` rereads every
matched byte and rechecks resolution, context and registry generation before a mutation. Passing the current snapshot
explicitly makes registry replacement observable. Helpers carry owned `localImports` as well as their statement body.
Fragments carry expected library module/defining-file identities for consumers checking user-source shadowing.

The shipped BOSL2 profile is 2.0.766, revision `e173fa0ae45f9e2082e0d8c0382dae98621b7c20` from
[BelfrySCAD/BOSL2](https://github.com/BelfrySCAD/BOSL2/tree/e173fa0ae45f9e2082e0d8c0382dae98621b7c20).
The BOLTS profile identifies the official [0.4.1 OpenSCAD release](https://github.com/boltsparts/BOLTS_archive/releases/tag/v0.4.1)
by its 41-file fingerprint; its internal marker says `development`, dated 2021-09-13.
The researched archive SHA-256 is `1b5656d984ef8019ff78ae5bfdf44baea5e2daab73e25c1e992d96dc8ae990b1`.
Catalog call/frame metadata and hashes were authored from these pinned inputs. Vendor SCAD implementations, dimension
tables and external test libraries are not redistributed. BOLTS's supplied source carries LGPL 2.1; its generated
hardware has simplified threads. BOSL2's hole uses modeled threads, tolerance 6H, explicit `$slop=0` and its pinned
lead-in extension: M4 nominal depth 12 produces a mask ending at Z=12.01.

The registry implements generation and diagnostics. Source mutation, placement, preview, undo and an insertion palette
belong to its consumers.
