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
#include "gui/vim/engine/TextObjects.h"
#include "gui/vim/engine/WordMotion.h"
#include <vector>
#include <algorithm>

namespace Vim {
Range textObject(const ScintillaAccess& s, const CommandParams& p, bool inner)
{
  if (p.argument == "w") return wordRange(s, p.pos, p.count, inner);
  if (p.argument.size() != 1) return {};
  const QChar key = p.argument.front();
  char32_t open = 0, close = 0;
  if (key == '(' || key == ')') { open = '('; close = ')'; }
  if (key == '[' || key == ']') { open = '['; close = ']'; }
  if (key == '{' || key == '}') { open = '{'; close = '}'; }
  if (key == '\'' || key == '"') open = close = key.unicode();
  if (!open) return {};
  std::vector<Position> stack;
  std::vector<Range> enclosing;
  char32_t quote = 0;
  bool escaped = false;
  const bool quotedObject = open == close;
  const auto begin = quotedObject ? s.start(p.line) : 0;
  const auto end = quotedObject ? s.end(p.line) : s.length();
  for (auto pos = begin; pos < end; pos = s.next(pos)) {
    const auto ch = s.character(pos);
    if (escaped) { escaped = false; continue; }
    if (ch == '\\' && (quote || quotedObject)) { escaped = true; continue; }
    if (quotedObject) {
      if (ch != open) continue;
      if (stack.empty()) stack.push_back(pos);
      else {
        const auto start = stack.back(); stack.pop_back();
        if (start <= p.pos && p.pos <= pos) enclosing.push_back({start, s.next(pos), false, true});
      }
    } else {
      if (quote) { if (ch == quote) quote = 0; continue; }
      if (ch == '\'' || ch == '"') { quote = ch; continue; }
      if (ch == open) stack.push_back(pos);
      else if (ch == close) {
        if (stack.empty()) continue;
        const auto start = stack.back(); stack.pop_back();
        if (start <= p.pos && p.pos <= pos) enclosing.push_back({start, s.next(pos), false, true});
      }
    }
  }
  if (enclosing.size() < p.count) return {};
  std::sort(enclosing.begin(), enclosing.end(), [](const auto& a, const auto& b) { return a.end - a.begin < b.end - b.begin; });
  auto result = enclosing[p.count - 1];
  if (inner) { result.begin = s.next(result.begin); result.end = s.prev(result.end); }
  return result;
}
}
