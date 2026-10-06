/*
 * Copyright 2019 Jiri Techet <techet@gmail.com>
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
#include "gui/vim/engine/WordMotion.h"
#include <QChar>
#include <algorithm>

namespace Vim {
// Geany motion-word.c's group-skipping algorithm, with Unicode scalar predicates.
int wordClass(char32_t c)
{
  if (!c || QChar::isSpace(c)) return 0;
  const auto category = QChar::category(c);
  return c == '_' || QChar::isLetterOrNumber(c) || category == QChar::Mark_NonSpacing ||
    category == QChar::Mark_SpacingCombining || category == QChar::Mark_Enclosing ? 1 : 2;
}
Position findNextWord(const ScintillaAccess& s, Position p, std::uint64_t n)
{
  while (n-- && p < s.length()) {
    const int group = wordClass(s.character(p));
    while (p < s.length() && wordClass(s.character(p)) == group) p = s.next(p);
    while (p < s.length() && wordClass(s.character(p)) == 0) p = s.next(p);
  }
  return p;
}
Position findPreviousWord(const ScintillaAccess& s, Position p, std::uint64_t n)
{
  while (n-- && p > 0) {
    p = s.prev(p);
    while (p > 0 && wordClass(s.character(p)) == 0) p = s.prev(p);
    const int group = wordClass(s.character(p));
    while (p > 0 && wordClass(s.character(s.prev(p))) == group) p = s.prev(p);
  }
  return p;
}
Position findNextWordEnd(const ScintillaAccess& s, Position p, std::uint64_t n)
{
  while (n-- && p < s.length()) {
    p = s.next(p);
    while (p < s.length() && wordClass(s.character(p)) == 0) p = s.next(p);
    if (p >= s.length()) return s.prev(s.length());
    const int group = wordClass(s.character(p));
    while (s.next(p) < s.length() && wordClass(s.character(s.next(p))) == group) p = s.next(p);
  }
  return p;
}
Range wordRange(const ScintillaAccess& s, Position p, std::uint64_t n, bool inner)
{
  if (p >= s.length()) return {};
  Position a = p, b = p;
  const int group = wordClass(s.character(p));
  while (a > 0 && wordClass(s.character(s.prev(a))) == group) a = s.prev(a);
  while (b < s.length() && wordClass(s.character(b)) == group) b = s.next(b);
  while (--n && b < s.length()) {
    while (b < s.length() && wordClass(s.character(b)) == 0) b = s.next(b);
    const int nextGroup = wordClass(s.character(b));
    while (b < s.length() && wordClass(s.character(b)) == nextGroup) b = s.next(b);
  }
  if (!inner && group != 0) {
    const auto old = b;
    while (b < s.end(s.line(b)) && wordClass(s.character(b)) == 0) b = s.next(b);
    if (b == old) while (a > s.start(s.line(a)) && wordClass(s.character(s.prev(a))) == 0) a = s.prev(a);
  }
  return {a, b, false, true};
}
}
