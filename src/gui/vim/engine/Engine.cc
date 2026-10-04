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
#include "gui/vim/engine/Search.h"
#include "gui/vim/engine/Motion.h"
#include "gui/vim/engine/TextObjects.h"
#include "gui/vim/engine/WordMotion.h"
#include <algorithm>

namespace Vim {
Register& unnamedRegister() { static Register value; return value; }
Engine::Engine(QsciScintilla *widget) : sci(widget)
{
  context.defaultCaret = sci.send(S::SCI_GETCARETSTYLE);
  context.defaultPeriod = sci.send(S::SCI_GETCARETPERIOD);
}
Engine::~Engine() { endUndo(sci, context); }
void Engine::callbacks(std::function<void()> changed, std::function<void(QChar)> prompt,
                       std::function<HostResult(const HostRequest&)> host)
{
  context.changed = std::move(changed); context.prompt = std::move(prompt); context.host = std::move(host);
}
void Engine::status(const QString& value)
{
  context.feedback = value;
  if (context.changed) context.changed();
}
void Engine::setEnabled(bool enabled)
{
  if (context.enabled == enabled) return;
  cancelPending();
  context.enabled = enabled; context.mode = Mode::Normal;
  sci.send(S::SCI_SETCARETSTYLE, enabled ? S::CARETSTYLE_BLOCK : context.defaultCaret);
  sci.send(S::SCI_SETCARETPERIOD, enabled ? 0 : context.defaultPeriod);
  if (enabled) sci.position(sci.clamp(sci.pos()));
  status({});
}
void Engine::cancelPending()
{
  QString insertionFeedback;
  context.keys.clear(); context.desiredColumn = -1;
  context.rejectedCount = false; context.rejectedKeys.clear();
  if (context.mode == Mode::Insert) { finishInsert(true, true); insertionFeedback = context.feedback; }
  else if (context.mode != Mode::Normal) {
    sci.position(sci.clamp(context.caret)); context.mode = Mode::Normal;
    sci.send(S::SCI_SETCARETSTYLE, S::CARETSTYLE_BLOCK);
  }
  status(insertionFeedback);
}
void Engine::beforeExternalMutation()
{
  if (!context.enabled || context.ownedEdit) return;
  // Called synchronously by 02 BEFORE the external caller begins an undo action.
  if (context.mode == Mode::Insert) finishInsert(false, false);
  context.keys.clear(); context.mode = Mode::Normal;
  context.desiredColumn = -1;
  if (context.enabled) sci.send(S::SCI_SETCARETSTYLE, S::CARETSTYLE_BLOCK);
  status({});
}
bool Engine::claimsShortcut(const Key& key) const
{
  if (!context.enabled) return false;
  if (context.mode != Mode::Insert) return !commandKey(key).isEmpty();
  if (printable(key) || key.qtKey == Qt::Key_Escape) return true;
  if (key.modifiers == Qt::ControlModifier &&
      (key.qtKey == Qt::Key_V || key.qtKey == Qt::Key_Backspace || key.qtKey == Qt::Key_Delete)) return true;
  if (key.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
  return key.qtKey == Qt::Key_Backspace || key.qtKey == Qt::Key_Delete || key.qtKey == Qt::Key_Return ||
    key.qtKey == Qt::Key_Enter || key.qtKey == Qt::Key_Tab || key.qtKey == Qt::Key_Left ||
    key.qtKey == Qt::Key_Right || key.qtKey == Qt::Key_Up || key.qtKey == Qt::Key_Down ||
    key.qtKey == Qt::Key_Home || key.qtKey == Qt::Key_End;
}
InputResult Engine::key(const Key& key)
{
  if (!context.enabled) return InputResult::PassToEditor;
  const auto text = commandKey(key);
  if (text == "<escape>") {
    sci.send(S::SCI_AUTOCCANCEL); sci.send(S::SCI_CALLTIPCANCEL);
    cancelPending(); return InputResult::Consumed;
  }
  if (context.mode == Mode::Insert) return claimsShortcut(key) ? InputResult::PassToEditor : InputResult::PassToApplication;
  if (text.isEmpty()) return InputResult::PassToApplication;
  if (context.rejectedCount) {
    if (text.size() == 1 && text.front().isDigit()) return InputResult::Consumed;
    context.rejectedKeys += text;
    if (decode(context.rejectedKeys, context.mode != Mode::Normal).state != DecodeState::Pending) {
      context.rejectedCount = false; context.rejectedKeys.clear();
    }
    status("Count overflow; command refused"); return InputResult::Consumed;
  }
  context.keys += text;
  if (context.keys.size() > 64) { context.keys.clear(); status("Count or command is too long"); return InputResult::Consumed; }
  const auto decoded = decode(context.keys, context.mode != Mode::Normal);
  if (decoded.state == DecodeState::Pending) { status(context.keys); return InputResult::Consumed; }
  context.keys.clear();
  if (decoded.state == DecodeState::Unsupported) {
    if (decoded.command.name == "count-overflow") {
      context.rejectedCount = true;
      context.rejectedKeys = decoded.command.operation == QChar() ? QString() : QString(decoded.command.operation);
      status("Count overflow; command refused");
    } else status("Unsupported Vim command");
  }
  else execute(decoded.command);
  return InputResult::Consumed;
}
Range Engine::selection() const
{
  if (context.mode == Mode::VisualLine) return lineRange(sci, sci.line(context.anchor), sci.line(context.caret));
  const auto a = std::min(context.anchor, context.caret), b = std::max(context.anchor, context.caret);
  return {a, std::min(sci.next(b), sci.end(sci.line(b))), false, true};
}
void Engine::visualSelection()
{
  const auto range = selection();
  if (context.caret >= context.anchor) sci.send(S::SCI_SETSEL, range.begin, range.end);
  else sci.send(S::SCI_SETSEL, range.end, range.begin);
  sci.send(S::SCI_ENSUREVISIBLE, sci.line(context.caret)); sci.send(S::SCI_SCROLLCARET);
}
Range Engine::resolve(const Command& command)
{
  const auto pos = position(), line = sci.line(pos);
  CommandParams p{command.count, command.countPresent, pos, line, sci.start(line), sci.end(line), {}, command.motion, command.argument};
  if (command.motion.isEmpty() && context.mode != Mode::Normal) {
    if (command.operation == 'c' && context.mode == Mode::VisualLine) return lineRange(sci, sci.line(context.anchor), sci.line(context.caret), true);
    return selection();
  }
  if (command.motion == "line") {
    const auto last = std::min<std::uint64_t>(sci.send(S::SCI_GETLINECOUNT) - 1, line + std::min<std::uint64_t>(command.count - 1, sci.length()));
    return lineRange(sci, line, last, command.operation == 'c');
  }
  if (command.motion == "inner" || command.motion == "around") return textObject(sci, p, command.motion == "inner");
  // Geany operator/motion composition, repaired to give cw Vim's ce behavior.
  if (command.operation == 'c' && command.motion == "w" && wordClass(sci.character(pos)) != 0) {
    const auto current = wordRange(sci, pos, 1, true);
    auto last = sci.prev(current.end);
    if (command.count > 1) last = findNextWordEnd(sci, last, std::min<std::uint64_t>(command.count - 1, sci.length()));
    return {pos, sci.next(last), false, true};
  }
  const auto result = move(sci, context, p);
  if (!result.valid) return {};
  if (result.linewise) return lineRange(sci, line, sci.line(result.destination), command.operation == 'c');
  const auto a = std::min(pos, result.destination), b = std::max(pos, result.destination);
  auto end = result.inclusive ? sci.next(b) : b;
  if (command.motion == "w" && result.destination > pos && sci.line(result.destination) > line) {
    const auto previousLine = sci.line(sci.prev(result.destination));
    if (result.destination <= sci.nonempty(sci.line(result.destination))) end = sci.end(previousLine);
  }
  return {a, end, false, true};
}
void Engine::modified(Position pos, int type, const char *text, Position length)
{
  if (!context.enabled) return;
  if (context.ownedEdit) {
    if (type & S::SC_PERFORMED_UNDO && type & (S::SC_MOD_BEFOREINSERT | S::SC_MOD_BEFOREDELETE)) context.undoPosition = pos;
    return;
  }
  if (type & S::SC_MOD_INSERTTEXT) {
    if (context.mode == Mode::Insert && context.recording && context.inputDepth && context.pendingInsertion && text) {
      auto entry = std::move(*context.pendingInsertion); context.pendingInsertion.reset();
      entry.inserted = QByteArray(text, length); context.journalBytes += entry.inserted.size();
      if (context.journalBytes > MaxGeneratedBytes) { context.recording = false; context.insertion.journal.clear(); status("Insertion exceeds repeat limit"); return; }
      context.insertion.journal.push_back(std::move(entry));
    }
    return;
  }
  if (!(type & (S::SC_MOD_BEFOREINSERT | S::SC_MOD_BEFOREDELETE))) return;
  if (type & (S::SC_PERFORMED_UNDO | S::SC_PERFORMED_REDO)) { beforeExternalMutation(); return; }
  if (context.mode != Mode::Insert || !context.recording || context.inputDepth == 0) {
    beforeExternalMutation(); return;
  }
  InsertEdit entry;
  entry.offset = sci.characters(context.insertOrigin, pos);
  if (type & S::SC_MOD_BEFOREDELETE) entry.removed = sci.range(pos, pos + length);
  else {
    // BEFOREINSERT has no inserted bytes in the installed QScintilla. Its paired
    // INSERTTEXT notification arrives within the same native input guard.
    context.pendingInsertion = std::move(entry);
    if (pos < context.insertOrigin) context.insertOrigin += length;
    return;
  }
  context.journalBytes += entry.removed.size() + entry.inserted.size();
  if (context.journalBytes > MaxGeneratedBytes) {
    context.recording = false; context.insertion.journal.clear(); status("Insertion exceeds repeat limit"); return;
  }
  context.insertion.journal.push_back(std::move(entry));
  if (pos < context.insertOrigin) {
    if (type & S::SC_MOD_BEFOREDELETE) context.insertOrigin -= std::min(length, context.insertOrigin - pos);
    else context.insertOrigin += length;
  }
}
void Engine::submitSearch(const QString& value, bool forward)
{
  const auto text = value.isEmpty() ? context.searchText : value;
  const auto result = search(sci, text, position(), forward, 1);
  if (text.isEmpty()) { status("No previous search"); return; }
  context.searchText = text; context.searchForward = forward;
  if (result.position < 0) { status("Pattern not found"); return; }
  sci.position(sci.clamp(result.position));
  context.mode = Mode::Normal;
  status(result.wrapped ? "Search wrapped" : QString());
}
void Engine::submitCommand(const QString& text)
{
  const auto command = documentCommand(text);
  if (!command) { status("Unsupported document command"); return; }
  if (!context.host) { status("Document command unavailable"); return; }
  // Callback may close this editor. Keep no member accesses after calling it.
  auto host = context.host;
  auto changed = context.changed;
  context.feedback.clear();
  if (changed) changed();
  host(*command);
}
}
