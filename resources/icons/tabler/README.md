# OpenSCAD UI icons

Stock outline artwork is from Tabler Icons (MIT), revision
`74929e50416e2b7c0abb8368cdc74bdcb2560ab6`. Original files and license are retained.
`mapping.json` maps OpenSCAD's existing semantic names to assets; aliases intentionally share artwork.

Custom artwork in `custom/` is OpenSCAD-owned, licensed under the same MIT terms as the accompanying set.
It uses the Tabler 24-unit grid, round caps/joins and 2-unit strokes (1.5 for small format badges).
Format badges are paths, independent of installed fonts. No generic export icon substitutes for a format.

CAD distinctions:

- Preview: two dashed overlapping solids; render: their resolved outline.
- Surface: a solid face; wireframe: visible and hidden edges; edge overlay: emphasized vertices.
- Thrown together: overlapping square/circle with intersection marks.
- Camera: a shared cube with solid near faces and dashed far faces, preserving six distinct views.
- Projection: parallel edges versus converging edges. Reset: return arrow around a cube.
- Scale markers: graduated axes; distance: measuring ruler; XYZ axes: three directed axes.
- Animation: single triangle/bar for a frame step, double triangle/bar for start/end.
- Preferences: stock category symbols; AI has its own sparkles symbol rather than the experimental flask.

Status colors come from the active QStyle standard message icons, not QPalette semantic roles (none exist).
The renderer selects the most frequent opaque saturated color in the native icon. If there is no colored
ink, it uses the current text foreground. High-contrast mode uses system foregrounds. Normal action icons
keep their palette colors; colored status outlines adjust native tint brightness only if needed for 3:1 contrast.
Native status is the default, with monochrome and colored Tabler alternatives.

Coverage includes Designer actions/buttons, dynamic animation/export icons, input assignment errors,
OctoPrint pixmaps, compile banners and application-owned message boxes. Branding, welcome backgrounds,
user image thumbnails, color/font drag previews and native shell/chrome artwork are intentionally excluded.
