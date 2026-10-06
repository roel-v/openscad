/*
 * Copyright 2018 Jiri Techet <techet@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
// Adapted for SCADMaxx in 2026; see ORIGIN.md.
#include "gui/vim/engine/Engine.h"
#include "gui/vim/engine/Undo.h"

namespace Vim {
void Engine::enterInsert(const QString& command, Position pos, std::uint64_t count)
{
  beginUndo(sci, context);
  context.mode = Mode::Insert;
  context.recording = true; context.insertOrigin = pos; context.insertCount = count;
  context.journalBytes = 0; context.insertion = {command, {}, true};
  context.pendingInsertion.reset();
  sci.position(pos); sci.send(S::SCI_SETCARETSTYLE, S::CARETSTYLE_LINE);
  sci.send(S::SCI_SETCARETPERIOD, context.defaultPeriod);
  status({});
}
void Engine::finishInsert(bool saveRepeat, bool moveCaret)
{
  if (context.mode != Mode::Insert) return;
  context.mode = Mode::Normal;
  bool valid = context.recording && (context.insertion.command.contains('c') ||
    context.insertion.command.endsWith('o') || context.insertion.command.endsWith('O') || !context.insertion.journal.empty());
  context.recording = false;
  context.pendingInsertion.reset();
  QString failure;
  if (saveRepeat && valid && context.insertCount > 1) {
    valid = repeatInsertionCount(); if (!valid) failure = context.feedback;
  }
  if (saveRepeat && valid) context.repeat = context.insertion;
  endUndo(sci, context);
  auto pos = sci.pos();
  if (moveCaret && pos > sci.start(sci.line(pos))) pos = sci.prev(pos);
  sci.position(sci.clamp(pos));
  sci.send(S::SCI_SETCARETSTYLE, context.enabled ? S::CARETSTYLE_BLOCK : context.defaultCaret);
  sci.send(S::SCI_SETCARETPERIOD, context.enabled ? 0 : context.defaultPeriod);
  status(failure);
}
}
