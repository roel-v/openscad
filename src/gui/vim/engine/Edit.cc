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
#include "gui/vim/engine/Motion.h"
#include "gui/vim/engine/Undo.h"
#include "gui/vim/engine/Search.h"
#include <algorithm>

namespace Vim {
bool Engine::execute(const Command& command, bool repeating)
{
  const auto name = command.name;
  if (name == "motion" || name == "object") {
    if (name == "object") {
      const auto range = resolve(command);
      if (!range.valid || range.end <= range.begin) { status("Text object not found"); return false; }
      context.mode = Mode::VisualChar;
      context.anchor = range.begin; context.caret = sci.prev(range.end); visualSelection();
    } else {
      const auto pos = position(), line = sci.line(pos);
      const auto result = move(sci, context, {command.count, command.countPresent, pos, line, sci.start(line), sci.end(line), {}, command.motion, command.argument});
      if (!result.valid) { status("Motion target not found"); return false; }
      if (context.mode != Mode::Normal) { context.caret = sci.clamp(result.destination); visualSelection(); }
      else sci.position(sci.clamp(result.destination));
    }
    status({}); return true;
  }
  if (name == "v" || name == "V") {
    const auto mode = name == "v" ? Mode::VisualChar : Mode::VisualLine;
    if (context.mode == mode) { cancelPending(); return true; }
    if (context.mode == Mode::Normal) context.anchor = context.caret = sci.clamp(sci.pos());
    context.mode = mode; visualSelection(); status({}); return true;
  }
  if (name == "u" || name == "redo") {
    if (sci.readOnly()) { status("Editor is read-only"); return false; }
    cancelPending(); history(sci, context, name == "redo", command.count); return true;
  }
  if (name == "/" || name == "?" || name == ":") {
    auto callback = context.prompt;
    if (callback) callback(name.front());
    return true;
  }
  if (name == "n" || name == "N") {
    const auto result = search(sci, context.searchText, position(), name == "n" ? context.searchForward : !context.searchForward, command.count);
    if (result.position < 0) { status("Pattern not found"); return false; }
    if (context.mode != Mode::Normal) { context.caret = sci.clamp(result.position); visualSelection(); }
    else sci.position(sci.clamp(result.position));
    status(result.wrapped ? "Search wrapped" : QString()); return true;
  }
  if (name == ".") return repeat(command.count);
  if (name == "operator" || name == "x") {
    auto range = name == "x" ? Range{sci.pos(), std::min(sci.relative(sci.pos(), std::min<std::uint64_t>(command.count, sci.length())), sci.end(sci.line(sci.pos()))), false, true} : resolve(command);
    if (!range.valid || (range.begin == range.end && command.operation != 'c')) { status("Empty or unavailable range"); return false; }
    const auto copied = sci.range(range.begin, range.end);
    if (command.operation == 'y') {
      unnamedRegister() = {copied, range.linewise};
      context.mode = Mode::Normal; sci.position(sci.clamp(range.begin)); status({}); return true;
    }
    if (sci.readOnly()) { status("Editor is read-only"); return false; }
    QString repeatKeys = command.keys;
    if (command.motion.isEmpty() && context.mode != Mode::Normal) {
      repeatKeys = range.linewise ? QString::number(sci.line(sci.prev(range.end)) - sci.line(range.begin) + 1) + command.operation + command.operation :
        command.operation + QString::number(std::max<Position>(1, QString::fromUtf8(copied).toUcs4().size())) + "l";
    }
    unnamedRegister() = {copied, range.linewise};
    // A final unterminated line has no following EOL to delete. Remove its
    // preceding separator too, while keeping only selected bytes in the register.
    auto editBegin = range.begin;
    if (command.operation == 'd' && range.linewise && range.end == sci.length() && range.begin > 0 &&
        !copied.endsWith('\n') && !copied.endsWith('\r'))
      editBegin = sci.end(sci.line(range.begin) - 1);
    beginUndo(sci, context); context.ownedEdit = true;
    sci.replace(editBegin, range.end, {}); sci.position(editBegin);
    context.ownedEdit = false;
    context.mode = Mode::Normal;
    if (command.operation == 'c') enterInsert(repeatKeys, range.begin);
    else {
      endUndo(sci, context);
      sci.position(range.linewise ? sci.nonempty(sci.line(editBegin)) : sci.clamp(range.begin));
      if (!repeating) context.repeat = {repeatKeys, {}, true};
      status({});
    }
    return true;
  }
  if (name == "p" || name == "P") {
    if (sci.readOnly()) { status("Editor is read-only"); return false; }
    const auto& reg = unnamedRegister();
    if (reg.text.isEmpty()) { status("Unnamed register is empty"); return false; }
    const bool missingEol = reg.linewise && !reg.text.endsWith('\n') && !reg.text.endsWith('\r');
    const auto unitBytes = reg.text.size() + (missingEol ? sci.eol().size() : 0);
    if (command.count > static_cast<std::uint64_t>(MaxGeneratedBytes / unitBytes)) { status("Paste exceeds limit"); return false; }
    auto bytes = missingEol ? (reg.text + sci.eol()).repeated(command.count - 1) + reg.text : reg.text.repeated(command.count);
    auto pos = sci.pos();
    const auto line = sci.line(pos);
    Position caret = pos;
    if (reg.linewise) {
      pos = name == "P" ? sci.start(line) : line + 1 < sci.send(S::SCI_GETLINECOUNT) ? sci.start(line + 1) : sci.length();
      if (pos == sci.length() && pos > sci.start(line)) { bytes.prepend(sci.eol()); caret = pos + sci.eol().size(); }
      else caret = pos;
      if (!bytes.endsWith('\n') && !bytes.endsWith('\r') && pos < sci.length()) bytes += sci.eol();
    } else if (name == "p" && pos < sci.end(line)) pos = sci.next(pos);
    beginUndo(sci, context); context.ownedEdit = true; sci.replace(pos, pos, bytes);
    sci.position(reg.linewise ? sci.nonempty(sci.line(caret)) : sci.prev(pos + bytes.size()));
    context.ownedEdit = false; endUndo(sci, context);
    if (!repeating) context.repeat = {command.keys, {}, true};
    context.mode = Mode::Normal; status({}); return true;
  }
  if (QString("iaIAoO").contains(name) && name.size() == 1) {
    if (sci.readOnly()) { status("Editor is read-only"); return false; }
    const auto line = sci.line(sci.pos()); auto pos = sci.pos();
    if (name == "a") pos = std::min(sci.next(pos), sci.end(line));
    if (name == "I") pos = sci.nonempty(line);
    if (name == "A") pos = sci.end(line);
    if (name == "o" || name == "O") {
      beginUndo(sci, context); context.ownedEdit = true;
      pos = name == "o" ? sci.end(line) : sci.start(line);
      const auto indent = sci.autoIndent() ? sci.range(sci.start(line), sci.nonempty(line)) : QByteArray();
      sci.replace(pos, pos, name == "o" ? sci.eol() + indent : indent + sci.eol());
      pos += indent.size() + (name == "o" ? sci.eol().size() : 0);
      context.ownedEdit = false;
    }
    enterInsert(command.keys, pos, command.count); return true;
  }
  status("Unsupported Vim command"); return false;
}

namespace {
bool boundary(const QByteArray& text, Position p)
{
  return p >= 0 && p <= text.size() && (p == text.size() || (static_cast<unsigned char>(text[p]) & 0xc0) != 0x80);
}
Position after(const QByteArray& text, Position p)
{
  if (p >= text.size()) return text.size();
  ++p;
  while (p < text.size() && (static_cast<unsigned char>(text[p]) & 0xc0) == 0x80) ++p;
  return p;
}
Position before(const QByteArray& text, Position p)
{
  if (p <= 0) return 0;
  --p;
  while (p > 0 && (static_cast<unsigned char>(text[p]) & 0xc0) == 0x80) --p;
  return p;
}
Position offset(const QByteArray& text, Position p, Position n)
{
  while (n > 0 && p < text.size()) { p = after(text, p); --n; }
  while (n < 0 && p > 0) { p = before(text, p); ++n; }
  return n == 0 ? p : -1;
}
bool journal(QByteArray& text, Position& origin, Position& caret, const std::vector<InsertEdit>& entries)
{
  for (const auto& entry : entries) {
    const auto pos = offset(text, origin, entry.offset);
    if (pos < 0 || !boundary(text, pos)) return false;
    const auto end = offset(text, pos, QString::fromUtf8(entry.removed).toUcs4().size());
    if (end < 0 || !boundary(text, end)) return false;
    if (entry.inserted.size() > MaxGeneratedBytes) return false;
    text.replace(pos, end - pos, entry.inserted);
    if (pos < origin) origin += entry.inserted.size() - std::min<Position>(end - pos, origin - pos);
    caret = pos + entry.inserted.size();
  }
  return true;
}
}

bool Engine::repeat(std::uint64_t count)
{
  if (!context.repeat.valid) { status("No previous change"); return false; }
  if (sci.readOnly()) { status("Editor is read-only"); return false; }
  auto decoded = decode(context.repeat.command, false);
  if (decoded.state != DecodeState::Ready) { status("Previous change cannot be repeated"); return false; }
  auto command = decoded.command;
  if (!checkedMultiply(command.count, count, command.count)) { status("Repeat count overflow"); return false; }
  command.countPresent = command.countPresent || count != 1;
  const auto saved = context.repeat;
  if (saved.journal.empty() && command.operation != 'c' && command.name != "o" && command.name != "O") return execute(command, true);
  const auto original = sci.range(0, sci.length());
  auto planned = original;
  Position origin = sci.pos(), caret = origin;
  Register copied;
  bool writeRegister = false;
  if (command.operation == 'c') {
    const auto range = resolve(command);
    if (!range.valid) { status("Repeat target not found"); return false; }
    copied = {sci.range(range.begin, range.end), range.linewise}; writeRegister = true;
    planned.remove(range.begin, range.end - range.begin); origin = caret = range.begin;
  } else {
    const auto line = sci.line(origin);
    if (command.name == "a") origin = std::min(sci.next(origin), sci.end(line));
    if (command.name == "A") origin = sci.end(line);
    if (command.name == "I") origin = sci.nonempty(line);
    if (command.name == "o" || command.name == "O") {
      origin = command.name == "o" ? sci.end(line) : sci.start(line);
      const auto indent = sci.autoIndent() ? sci.range(sci.start(line), sci.nonempty(line)) : QByteArray();
      planned.insert(origin, command.name == "o" ? sci.eol() + indent : indent + sci.eol());
      origin += indent.size() + (command.name == "o" ? sci.eol().size() : 0);
    }
    caret = origin;
  }
  std::uint64_t repetitions = command.operation == 'c' ? 1 : command.count;
  const bool openLines = command.name == "o" || command.name == "O";
  const auto line = sci.line(sci.pos());
  const auto linePrefix = openLines ? sci.eol() + (sci.autoIndent() ? sci.range(sci.start(line), sci.nonempty(line)) : QByteArray()) : QByteArray();
  qsizetype generated = 0;
  for (const auto& entry : saved.journal) generated += entry.inserted.size() + entry.removed.size();
  generated += linePrefix.size();
  if (generated && repetitions > static_cast<std::uint64_t>(MaxGeneratedBytes / generated)) { status("Repeat exceeds limit"); return false; }
  bool first = true;
  while (repetitions--) {
    if (!first && openLines) { planned.insert(caret, linePrefix); caret += linePrefix.size(); origin = caret; }
    if (!journal(planned, origin, caret, saved.journal)) { status("Repeat insertion is unavailable here"); return false; }
    origin = caret; first = false;
  }
  // Apply only the changed UTF-8 interval, after the entire replay has succeeded.
  Position a = 0, oldEnd = original.size(), newEnd = planned.size();
  while (a < oldEnd && a < newEnd && original[a] == planned[a]) ++a;
  while (a > 0 && (!boundary(original, a) || !boundary(planned, a))) --a;
  while (oldEnd > a && newEnd > a && original[oldEnd - 1] == planned[newEnd - 1]) { --oldEnd; --newEnd; }
  while (oldEnd < original.size() && (!boundary(original, oldEnd) || !boundary(planned, newEnd))) { ++oldEnd; ++newEnd; }
  if (oldEnd == a && newEnd == a) { status({}); return true; }
  beginUndo(sci, context); context.ownedEdit = true;
  sci.replace(a, oldEnd, planned.mid(a, newEnd - a));
  if (writeRegister) unnamedRegister() = std::move(copied);
  context.mode = Mode::Normal;
  sci.position(sci.clamp(caret > sci.start(sci.line(caret)) ? sci.prev(caret) : caret));
  context.ownedEdit = false; endUndo(sci, context);
  status({}); return true;
}
bool Engine::repeatInsertionCount()
{
  const auto original = sci.range(0, sci.length());
  auto planned = original;
  auto origin = sci.pos(), caret = origin;
  auto count = context.insertCount - 1;
  const auto decoded = decode(context.insertion.command, false);
  const bool openLines = decoded.command.name == "o" || decoded.command.name == "O";
  const auto line = sci.line(context.insertOrigin);
  const auto linePrefix = openLines ? sci.eol() + (sci.autoIndent() ? sci.range(sci.start(line), sci.nonempty(line)) : QByteArray()) : QByteArray();
  qsizetype generated = 0;
  for (const auto& entry : context.insertion.journal) generated += entry.inserted.size() + entry.removed.size();
  generated += linePrefix.size();
  if (!generated) return true;
  if (count > static_cast<std::uint64_t>(MaxGeneratedBytes / generated)) { status("Counted insertion exceeds limit"); return false; }
  while (count--) {
    if (openLines) { planned.insert(caret, linePrefix); caret += linePrefix.size(); origin = caret; }
    if (!journal(planned, origin, caret, context.insertion.journal)) { status("Counted insertion is unavailable here"); return false; }
    origin = caret;
  }
  Position begin = 0, oldEnd = original.size(), newEnd = planned.size();
  while (begin < oldEnd && begin < newEnd && original[begin] == planned[begin]) ++begin;
  while (begin && (!boundary(original, begin) || !boundary(planned, begin))) --begin;
  while (oldEnd > begin && newEnd > begin && original[oldEnd - 1] == planned[newEnd - 1]) { --oldEnd; --newEnd; }
  while (oldEnd < original.size() && (!boundary(original, oldEnd) || !boundary(planned, newEnd))) { ++oldEnd; ++newEnd; }
  context.ownedEdit = true; sci.replace(begin, oldEnd, planned.mid(begin, newEnd - begin)); sci.position(caret); context.ownedEdit = false;
  return true;
}
}
