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
#include <Qsci/qsciscintilla.h>
#include <QPointer>
#include <QByteArray>
#include <cstdint>

namespace Vim {
using Position = std::intptr_t; // Scintilla's sptr_t/Sci_Position ABI, including Win64.
using S = QsciScintillaBase;
class ScintillaAccess {
  QPointer<QsciScintilla> widget;
  using DirectFunction = Position (*)(Position, unsigned int, std::uintptr_t, Position);
  DirectFunction direct;
  Position instance;
public:
  explicit ScintillaAccess(QsciScintilla *);
  Position send(unsigned int message, std::uintptr_t w = 0, Position l = 0) const;
  Position length() const { return send(S::SCI_GETLENGTH); }
  Position pos() const { return send(S::SCI_GETCURRENTPOS); }
  Position next(Position p) const { return send(S::SCI_POSITIONAFTER, p); }
  Position prev(Position p) const { return send(S::SCI_POSITIONBEFORE, p); }
  Position line(Position p) const { return send(S::SCI_LINEFROMPOSITION, p); }
  Position start(Position line) const { return send(S::SCI_POSITIONFROMLINE, line); }
  Position end(Position line) const { return send(S::SCI_GETLINEENDPOSITION, line); }
  Position relative(Position p, Position n) const;
  Position characters(Position a, Position b) const;
  QByteArray range(Position a, Position b) const;
  char32_t character(Position p) const;
  void position(Position p, bool scroll = true) const;
  Position clamp(Position p) const;
  Position nonempty(Position line) const;
  bool readOnly() const { return send(S::SCI_GETREADONLY) != 0; }
  bool autoIndent() const { return widget && widget->autoIndent(); }
  void replace(Position a, Position b, const QByteArray&) const;
  QByteArray eol() const;
};
}
