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
#include "gui/vim/engine/Motion.h"
#include "gui/vim/engine/WordMotion.h"
#include <algorithm>

namespace Vim {
Range lineRange(const ScintillaAccess& s, Position first, Position last, bool change)
{
  if (last < first) std::swap(first, last);
  last = std::min(last, s.send(S::SCI_GETLINECOUNT) - 1);
  const auto a = change && s.autoIndent() ? s.nonempty(first) : s.start(first);
  const auto b = change ? s.end(last) : last + 1 < s.send(S::SCI_GETLINECOUNT) ? s.start(last + 1) : s.length();
  return {a, b, true, true};
}
MotionResult move(const ScintillaAccess& s, Context& c, const CommandParams& p)
{
  auto destination = p.pos;
  const auto n = std::min<std::uint64_t>(p.count, s.length() + 1);
  const auto m = p.motion;
  bool inclusive = false, linewise = false;
  if (m == "h") {
    for (std::uint64_t i = 0; i < n && destination > p.lineStart; ++i) destination = s.prev(destination);
  } else if (m == "l") {
    for (std::uint64_t i = 0; i < n && destination < p.lineEnd; ++i) destination = s.next(destination);
  } else if (m == "j" || m == "k") {
    if (c.desiredColumn < 0) c.desiredColumn = s.characters(p.lineStart, p.pos);
    const auto line = m == "j" ? std::min<Position>(p.line + n, s.send(S::SCI_GETLINECOUNT) - 1) :
      std::max<Position>(0, p.line - static_cast<Position>(n));
    destination = std::min(s.relative(s.start(line), c.desiredColumn), s.end(line));
    linewise = true;
  } else if (m == "0") destination = p.lineStart;
  else if (m == "^") destination = s.nonempty(p.line);
  else if (m == "$") {
    const auto line = std::min<Position>(p.line + n - 1, s.send(S::SCI_GETLINECOUNT) - 1);
    destination = s.clamp(s.end(line)); inclusive = true;
  } else if (m == "gg" || m == "G") {
    const auto line = p.countPresent ? std::min<std::uint64_t>(p.count, s.send(S::SCI_GETLINECOUNT)) - 1 :
      m == "G" ? s.send(S::SCI_GETLINECOUNT) - 1 : 0;
    destination = s.nonempty(line); linewise = true;
  } else if (m == "w") destination = findNextWord(s, p.pos, n);
  else if (m == "b") destination = findPreviousWord(s, p.pos, n);
  else if (m == "e") { destination = findNextWordEnd(s, p.pos, n); inclusive = true; }
  else if (m == "f" || m == "F" || m == "t" || m == "T" || m == ";" || m == ",") {
    QChar kind; QString text;
    if (m == ";" || m == ",") { kind = c.findKind; text = c.findText; }
    else { kind = m.front(); text = p.argument; }
    if (text.isEmpty() || text.toUcs4().size() != 1) return {};
    bool forward = kind == 'f' || kind == 't';
    if (m == ",") forward = !forward;
    auto found = p.pos;
    if ((m == ";" || m == ",") && (kind == 't' || kind == 'T')) found = forward ? s.next(found) : s.prev(found);
    for (std::uint64_t i = 0; i < n; ++i) {
      bool matched = false;
      while (forward ? found < p.lineEnd : found > p.lineStart) {
        found = forward ? s.next(found) : s.prev(found);
        if (found < p.lineEnd && s.range(found, s.next(found)) == text.toUtf8()) { matched = true; break; }
      }
      if (!matched) return {};
    }
    destination = kind == 't' || kind == 'T' ? (forward ? s.prev(found) : s.next(found)) : found;
    inclusive = forward;
    if (m != ";" && m != ",") { c.findKind = kind; c.findText = text; }
  } else return {};
  if (m != "j" && m != "k") c.desiredColumn = -1;
  return {destination, inclusive, linewise, true};
}
}
