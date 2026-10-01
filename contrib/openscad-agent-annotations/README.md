# Native OpenSCAD annotations

Open **Window > Annotations**, preview or render a document, then choose **Annotate issue**.
Draw with the left mouse button and choose from twelve colors in the fixed palette.
The cursor becomes an arrow over the palette and returns to a crosshair over the drawing area.
Navigate before drawing. Once there are marks, the camera is locked. Undo all marks to navigate again;
navigation discards draft redo. The standard Undo/Redo shortcuts operate on draft strokes while drawing.

Press Enter to capture the viewport and add an optional comment. **Back** returns to drawing,
**Cancel** discards the draft, and **Add** accepts it. Empty annotations are allowed. Escape or right-click
cancels drawing. Resizing or changing the source/result cancels a draft to avoid misaligned evidence.
The captured PNG excludes the palette and contains the strokes exactly once.

The dock lists newest annotations first and supports comment search, comment editing and confirmed deletion.
Selecting an annotation only displays its historical image. **Restore view here** explicitly restores its camera
against the current geometry. Historical strokes never appear on the live model.

Annotations belong to the displayed model's document, which can differ from the active editor tab. The dock shows
that owner. **Save owner document** saves it explicitly. Save and undo from the dock/viewport also target that owner.
Accepted annotation edits join the source editor's ordinary undo history.

Save keeps annotations in `model.scad.annotations` alongside `model.scad`. Keep both files when moving a design.
Save As and Save a Copy include evidence; accepting an annotation alone does not save. Annotation-only Save leaves
the SCAD file untouched. Automatic source reload retains annotations and their dirty state, while clearing both
source and annotation undo history. Backups contain matching SCAD/annotation pairs.

Agent access is optional. Enable **local agent access** in the dock, and use **Copy connection info** or the
Python client's discovery command. **Stop agent access** immediately ends the service. The bundled
[agent workflow](SKILL.md) explains explicit target selection, captures and normal external SCAD edits.
There is no source-edit, save, compile, document-open or document-switching API.

This implementation has been built and tested on Windows x64 with Qt6/MSVC/vcpkg. The repository-local feature
plans and run evidence record the precise build inputs and verification limits.
