# Inspection foundation

The geometric inspection foundation is window-owned and independent of source provenance, semantic ownership,
design objects and clipping. `EvaluationBinding` uses the existing document/result identities and owns an immutable
`EvaluatedScene`. Its backing retains exact captured UTF-8 root/include/use bytes, parsed ASTs, source caches and
evaluation contexts. Workers retain this shared scene, never MainWindow's mutable tree or global source cache.

`SelectionService::pick(logicalPixel, stamp)` reads visible geometry in two GPU passes. A hit contains a result-local
64-bit ObjectKey, world point and outward normal, with optional invertible local frame/point. Final aggregate meshes
identify the rendered root; they do not promise a source instance or face. OpenCSG preview, Manifold/PolySet final and
simple convertible CGAL final are supported. Thrown Together and 2D are unavailable; empty geometry is a miss.
Transparent surfaces are opaque for picking. Background geometry is excluded. Compact GPU tokens reserve zero and
use 24 bits; semantic draw IDs remain 64 bits. Required formats are depth24/stencil8 and RGBA32F with OpenGL 3 support.

Pass positions use the captured P*M, physical pixel centers and one explicit recentering origin. VBOBuilder's negative
face-cross normal convention, subtraction polarity and mirrored winding are corrected once by each renderer state.
Decorators must preserve this convention. A pick rejects changes to source/configuration, active document, renderer
generation, camera or presentation before delivery. Surface shaders are installed with the ordinary shader resources.

## Consumer interfaces

- `ViewportToolRouter::beginPick` arms a one-shot tool; a miss remains armed. Completion or cancellation retires it
  before calling the consumer. `beginDrag` captures a chosen mouse button and delivers one release or cancellation.
  Navigation drags and wheel/cube input remain available while a pick is armed. `claimExternal` coordinates legacy
  annotation and measurement handlers. QObject ownership, tab/result/focus changes and Escape govern cancellation.
- `ViewportOverlays::registerOverlay` returns a move-only QObject-scoped token. Layers are WorldDepth, WorldOverlay,
  HUD; order then registration sequence are deterministic. Every callback gets a captured camera/stamp and a GL state
  guard. Helpers are excluded from surface picking and clean model capture.
- `InspectorDock::registerContribution` owns a contribution widget and replaces it on selection changes. IDs are
  unique; order then ID determines layout. QObject teardown unregisters the callback and destroys its widget.
  Enrichment observers connect to `SelectionService::selectionChanged` with their QObject lifetime, and retain their
  own typed result keyed by Stamp/ObjectKey. The geometric state is not a semantic/provenance store.

Tokens and controller mutation run on the GUI thread. Callbacks that may outlive their QObject must capture QPointer
or immutable values, especially cancellation on OwnerDestroyed. Widget builders return a newly owned widget.

## Shared inspection presentation

`InspectionPresentationController` is the only owner of temporary mesh display installation/restoration. A provider
registration alone does not activate display. Active mesh requests share one highest-priority enabled provider job.
Duplicate provider IDs/priorities fail. Pending or failed higher-priority jobs never fall back to lower providers.
Provider starts receive a retained scene and stamp, return a cancellation function, and may deliver from a worker.
Delivery is queued through a retained GUI dispatcher; stale tickets, results and owner lifetimes cannot install.

Ready sets contain immutable local PolySets, affine transforms, colors/roles and unique nonzero draw IDs. The common
mesh renderer transforms each instance once before recentering/upload, retains exact piece identity and omits
background pieces from hits. Coincident equal-depth surfaces follow deterministic piece order; only the displayed
owner can be reported. Singular transforms have no local hit frame.

Each request may install a result/generation-bound mask. Missing mask means all visible; an empty set hides all;
multiple masks intersect. Masks reject unknown IDs and non-increasing serials. Mask changes and effect uniform
updates advance presentation serial without evaluating source or rebuilding meshes. Effect factories wrap the shared
child in order then ID and must apply one predicate to color, edge and hit passes. Provider/ready-set changes and final
restoration advance viewportGeneration under the same result ID. Consumers obtain fresh geometric keys after change;
their own source-bound gestures need not cancel for a uniform/mask change.

Preparation displays explicit Preparing inspection / Cancel and preserves last-good pixels and navigation. Surface
selection is unavailable until the selected provider is ready. Final request release restores the retained default.
No Section or Part-specific implementation is required or imported by this foundation.
