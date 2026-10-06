/*
 * Copyright 2024 Sylvain Cresto <scresto@gmail.com>
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
void beginUndo(const ScintillaAccess&, Context&);
void endUndo(const ScintillaAccess&, Context&);
void history(const ScintillaAccess&, Context&, bool redo, std::uint64_t count);
}
