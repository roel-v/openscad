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
#include "gui/vim/engine/QtKey.h"
#include <QList>
namespace Vim {
bool printable(const Key& k)
{
  const bool altGr = k.modifiers.testFlag(Qt::GroupSwitchModifier) ||
    (k.modifiers.testFlag(Qt::ControlModifier) && k.modifiers.testFlag(Qt::AltModifier));
  if (!altGr && (k.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return false;
  return !k.text.isEmpty() && k.text.toUcs4().front() >= 32;
}
QString commandKey(const Key& k)
{
  if (printable(k)) return k.text;
  if (k.modifiers.testFlag(Qt::ControlModifier) && k.qtKey == Qt::Key_R) return "<redo>";
  if (k.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return {};
  switch (k.qtKey) {
  case Qt::Key_Left: return "h";
  case Qt::Key_Right: return "l";
  case Qt::Key_Up: return "k";
  case Qt::Key_Down: return "j";
  case Qt::Key_Home: return "0";
  case Qt::Key_End: return "$";
  case Qt::Key_Backspace:
  case Qt::Key_Delete:
  case Qt::Key_Return:
  case Qt::Key_Enter:
  case Qt::Key_Tab: return "<unsupported-edit>";
  case Qt::Key_Escape: return "<escape>";
  default: return {};
  }
}
}
