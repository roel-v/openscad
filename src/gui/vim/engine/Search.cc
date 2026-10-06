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
#include "gui/vim/engine/Search.h"
#include <algorithm>

namespace Vim {
SearchResult search(const ScintillaAccess& s, const QString& pattern, Position pos, bool forward, std::uint64_t count)
{
  if (pattern.isEmpty()) return {};
  const auto bytes = pattern.toUtf8();
  // Geany perform_search's two targets, using literal search instead of its regex/substitution path.
  const auto find = [&](Position start, Position end) {
    s.send(S::SCI_SETSEARCHFLAGS, S::SCFIND_MATCHCASE); // no regexp flags
    s.send(S::SCI_SETTARGETSTART, start); s.send(S::SCI_SETTARGETEND, end);
    return s.send(S::SCI_SEARCHINTARGET, bytes.size(), reinterpret_cast<Position>(bytes.constData()));
  };
  bool wrapped = false;
  // Counts larger than the document cannot discover more unique positions; bound work.
  count = std::min<std::uint64_t>(count, s.length() + 1);
  while (count--) {
    auto result = forward ? find(s.next(pos), s.length()) : find(pos, 0);
    if (result < 0) {
      result = forward ? find(0, std::min<Position>(s.length(), pos + bytes.size())) : find(s.length(), pos);
      wrapped = true;
    }
    if (result < 0) return {};
    pos = result;
  }
  return {pos, wrapped};
}
std::optional<HostRequest> documentCommand(const QString& text)
{
  if (text == "w") return HostRequest{HostCommand::Save, {}};
  if (text == "q") return HostRequest{HostCommand::Close, {}};
  if (text == "e") return HostRequest{HostCommand::Reload, {}};
  if (!text.startsWith('e') || text.size() < 3 || !text[1].isSpace()) return {};
  qsizetype i = 2;
  while (i < text.size() && text[i].isSpace()) ++i;
  auto path = text.mid(i);
  if (path.isEmpty()) return {};
  if (path.startsWith('"')) {
    if (path.size() < 2 || !path.endsWith('"')) return {};
    path = path.mid(1, path.size() - 2);
  } else if (path.endsWith('"')) return {};
  if (path.isEmpty()) return {};
  return HostRequest{HostCommand::Open, path};
}
}
