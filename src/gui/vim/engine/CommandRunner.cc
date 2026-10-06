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
#include "gui/vim/engine/CommandRunner.h"
#include <limits>

namespace Vim {
bool checkedMultiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result)
{
  if (b && a > std::numeric_limits<std::uint64_t>::max() / b) return false;
  result = a * b; return true;
}
static bool count(const QString& keys, qsizetype& at, std::uint64_t& n, bool& present)
{
  n = 0; present = false;
  while (at < keys.size() && keys[at] >= '0' && keys[at] <= '9' && (present || keys[at] != '0')) {
    const auto digit = keys[at++].unicode() - '0';
    if (n > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
    n = n * 10 + digit; present = true;
  }
  if (!present) n = 1;
  return true;
}
Decoded decode(const QString& keys, bool visual)
{
  // Geany's allowlisted command tables: motion/selection is resolved before its operator.
  Command command; command.keys = keys;
  qsizetype at = 0;
  if (!count(keys, at, command.count, command.countPresent)) { command.name = "count-overflow"; return {DecodeState::Unsupported, command}; }
  if (at == keys.size()) return {DecodeState::Pending, {}};
  QString suffix = keys.mid(at);
  if (suffix == "<redo>") { command.name = "redo"; return {DecodeState::Ready, command}; }
  if (QString("dcy").contains(suffix.front())) {
    command.operation = suffix.front(); command.name = "operator"; ++at;
    if (visual && at == keys.size()) return {DecodeState::Ready, command};
    std::uint64_t second; bool secondPresent;
    if (!count(keys, at, second, secondPresent) || !checkedMultiply(command.count, second, command.count))
      { command.name = "count-overflow"; return {DecodeState::Unsupported, command}; }
    command.countPresent = command.countPresent || secondPresent;
    if (at == keys.size()) return {DecodeState::Pending, {}};
    suffix = keys.mid(at);
    if (suffix == command.operation) { command.motion = "line"; return {DecodeState::Ready, command}; }
  }
  if (suffix == "g") return {DecodeState::Pending, {}};
  if (suffix == "gg" || (suffix.size() == 1 && QString("hjklwbe0^$G;, ").contains(suffix))) {
    command.motion = suffix == " " ? "l" : suffix;
    if (command.name.isEmpty()) command.name = "motion";
    return {DecodeState::Ready, command};
  }
  if (QString("fFtT").contains(suffix.front())) {
    if (suffix.size() == 1) return {DecodeState::Pending, {}};
    command.motion = suffix.first(1); command.argument = suffix.mid(1);
    if (command.argument.toUcs4().size() != 1) return {DecodeState::Unsupported, {}};
    if (command.name.isEmpty()) command.name = "motion";
    return {DecodeState::Ready, command};
  }
  if ((command.operation != QChar() || visual) && (suffix.front() == 'i' || suffix.front() == 'a')) {
    if (suffix.size() == 1) return {DecodeState::Pending, {}};
    if (suffix.size() != 2 || !QString("w()[]{}\"'").contains(suffix[1])) return {DecodeState::Unsupported, {}};
    command.motion = suffix.first(1) == "i" ? "inner" : "around"; command.argument = suffix.mid(1);
    if (command.name.isEmpty()) command.name = "object";
    return {DecodeState::Ready, command};
  }
  if (command.operation != QChar()) return {DecodeState::Unsupported, {}};
  if (suffix.size() == 1 && QString("iaIAoOvVx pPu.nN/?:").contains(suffix)) {
    command.name = suffix; return {DecodeState::Ready, command};
  }
  return {DecodeState::Unsupported, {}};
}
}
