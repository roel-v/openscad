# Bounded Vim editor mode

The **Enable Vim editor mode** checkbox in Editor preferences is off by default.
It applies to every source editor, including new tabs/windows, and persists across
application restarts. The mode row below each editor shows Normal, Insert, Visual
or Visual Line; its tooltip lists the supported commands.

Normal and Visual use `h j k l`/arrows, `w b e`, `0 ^ $`, `gg G`, `f F t T`, `; ,`,
counts, `d c y` plus a supported motion/object, `dd cc yy`, `x`, `p P`, `u`, Ctrl+R
and dot repeat. `i a I A o O` enter Insert; `v V` select characterwise or linewise.
Inner/around word, parentheses, square/curly brackets and single/double quotes
work after an operator or in Visual. Escape cancels pending input and returns to
Normal, also dismissing a completion/call-tip without accepting it.

`/` and `?` open an editor-local, case-sensitive **literal** search prompt. Enter
searches, Escape restores the original caret/selection, `n` repeats and `N`
reverses. Search wraps once and reports missing patterns. An empty submitted
pattern reuses the previous pattern. Scintilla requires `SCFIND_MATCHCASE` for
this behavior; zero search flags would be case-insensitive.

The `:` prompt accepts exactly `w`, `q`, `e`, and `e path`. An outer pair of double
quotes is removed from a path; its spaces and Unicode are preserved. Save, close,
reload and open use the originating window's document manager and its existing
Save As, source-conflict and dirty-document protection. Reload of an untitled
document is unavailable. A failed/cancelled command is shown in the mode row.

Unsupported printable commands are consumed with feedback. Replace/Block modes,
macros, named registers, substitution, marks, broad Ex commands, force forms,
shell/configuration execution and user mappings are outside this implementation.
Read-only documents retain navigation and search. Ordinary application shortcuts
and editing menus remain available, and fields outside the source editor use
their ordinary bindings.

## Implementation boundary

The engine is a GPL-2.0-or-later C++ adaptation of pinned Geany vimode source;
see [its attribution and mapping](../src/gui/vim/engine/ORIGIN.md). The separate
`OpenSCADVimEngine` target uses Qt Widgets and QScintilla. It has no GTK/GLib
backend or shim; OpenSCAD's existing unrelated GLib use remains in its application
target. Qt host components own prompts and document callbacks.

Every editor owns its mode, pending input, search and repeat. Only the unnamed
UTF-8 byte register is process-wide. Ranges are half-open byte intervals and
motions use Scintilla character boundaries. Counts use checked arithmetic;
generated paste/repeat data is limited to 64 MiB per command, with refusal before
mutation. This internal bound has no preference.

Vim changes use the existing Scintilla undo history. A change plus Insert is one
undo action. Native key/IME/completion/paste input records actual insert/delete
notifications in a relative edit journal, including backspace and Unicode; dot
preflights that journal before applying a single changed interval. Native undo,
redo and dot never record themselves.

Feature 02's synchronous `externalEditStarting` hook closes and invalidates an
insertion journal **before** an external transaction, menu edit or annotation
container undo is grouped. Notification-only detection is a secondary check.
Focus loss/tab switching closes pending input for its own document; prompts and
callbacks use guarded originating-document pointers through modal dialogs.

Verification includes engine CTest fixtures and the Windows `TestVimMode` native
suite, rather than treating direct engine calls as proof of Qt shortcut routing.
`TestVimPersistence` supports a separate process-start preference probe. Build/run
records and source snapshots belong outside the upstream-facing checkout.
