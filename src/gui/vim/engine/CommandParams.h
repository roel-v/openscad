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
#pragma once
#include "gui/vim/engine/Context.h"
namespace Vim {
// Adapted CmdParams: explicit count and half-open range replace implicit selection length.
struct CommandParams {
  std::uint64_t count = 1;
  bool countPresent = false;
  Position pos = 0, line = 0, lineStart = 0, lineEnd = 0;
  Range selection;
  QString motion, argument;
};
bool checkedMultiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result);
}
