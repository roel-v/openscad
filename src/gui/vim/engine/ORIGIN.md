# Geany vimode Qt/QScintilla adaptation

Pinned source: geany/geany-plugins `8d1838869cfdebbeb8469193119839f7ffae0052`, `vimode/src`.
License: GPL-2.0-or-later. Original per-file notices and plugin LICENSE are retained.

| Original family | Adapted files |
| --- | --- |
| vi.c/.h, context.h | Engine.cc/.h, Context.h |
| cmd-runner.c/.h, cmd-params.c/.h | CommandRunner.cc/.h, CommandParams.h |
| cmds/motion.c/.h | Motion.cc/.h |
| cmds/motion-word.c/.h | WordMotion.cc/.h |
| cmds/txtobjs.c/.h | TextObjects.cc/.h |
| cmds/edit.c/.h | Edit.cc |
| cmds/changemode.c/.h | Modes.cc |
| cmds/undo.c/.h | Undo.cc/.h |
| sci.c/.h, utils.c/.h | ScintillaAccess.cc/.h, Search.cc/.h |
| keypress.c/.h | QtKey.cc/.h |

This port retains the operator/motion command grouping, parameterized counts, character-group
word scanners, character-find direction/till behavior, selected text-object and native-undo
algorithms. It replaces toolkit types, key translation and message transport directly with
Qt/C++ and Scintilla's pointer-sized direct API. State is owned by each editor, with only the
unnamed byte register shared. Counts, half-open UTF-8 ranges, inclusive Visual selections,
literal search and modification journals repair limitations in the original implementation.

The original global active editor, fixed insertion array and SCN_CHARADDED replay are removed.
The Geany/viw backends, toolkit prompts, broad Ex runner, substitution, folds, Replace mode,
case/indent commands, marks, macros and named registers are excluded from the compiled sources.
Host document commands and prompts are native Qt components outside this adapted engine.
