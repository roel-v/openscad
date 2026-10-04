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
#include "gui/vim/engine/ScintillaAccess.h"
#include <QChar>
#include <QString>
#include <algorithm>
#include <stdexcept>

namespace Vim {
ScintillaAccess::ScintillaAccess(QsciScintilla *sci) : widget(sci),
  direct(reinterpret_cast<DirectFunction>(sci->SendScintillaPtrResult(S::SCI_GETDIRECTFUNCTION))),
  instance(reinterpret_cast<Position>(sci->SendScintillaPtrResult(S::SCI_GETDIRECTPOINTER)))
{
  static_assert(sizeof(Position) == sizeof(void *));
  if (!direct || !instance) throw std::runtime_error("Scintilla direct interface unavailable");
}
Position ScintillaAccess::send(unsigned int m, std::uintptr_t w, Position l) const
{
  return widget ? direct(instance, m, w, l) : 0;
}
Position ScintillaAccess::characters(Position a, Position b) const
{
  return a <= b ? send(S::SCI_COUNTCHARACTERS, a, b) : -send(S::SCI_COUNTCHARACTERS, b, a);
}
Position ScintillaAccess::relative(Position p, Position n) const
{
  // POSITIONRELATIVE returns zero beyond EOF in the installed Scintilla; clamp first.
  n = std::clamp<Position>(n, -characters(0, p), characters(p, length()));
  return send(S::SCI_POSITIONRELATIVE, p, n);
}
QByteArray ScintillaAccess::range(Position a, Position b) const
{
  a = std::clamp<Position>(a, 0, length()); b = std::clamp<Position>(b, a, length());
  const auto bytes = reinterpret_cast<const char *>(send(S::SCI_GETRANGEPOINTER, a, b - a));
  return bytes ? QByteArray(bytes, b - a) : QByteArray();
}
char32_t ScintillaAccess::character(Position p) const
{
  if (p < 0 || p >= length()) return 0;
  const auto text = QString::fromUtf8(range(p, next(p))).toUcs4();
  return text.isEmpty() ? 0 : text.front();
}
void ScintillaAccess::position(Position p, bool scroll) const
{
  p = std::clamp<Position>(p, 0, length());
  send(S::SCI_SETEMPTYSELECTION, p);
  send(S::SCI_ENSUREVISIBLE, line(p));
  if (scroll) send(S::SCI_SCROLLCARET);
  send(S::SCI_CHOOSECARETX);
}
Position ScintillaAccess::clamp(Position p) const
{
  p = std::clamp<Position>(p, 0, length());
  const auto a = start(line(p)), b = end(line(p));
  return p >= b && b > a ? prev(b) : p;
}
Position ScintillaAccess::nonempty(Position l) const
{
  auto p = start(l); const auto b = end(l);
  while (p < b && QChar::isSpace(character(p))) p = next(p);
  return p;
}
void ScintillaAccess::replace(Position a, Position b, const QByteArray& text) const
{
  send(S::SCI_SETTARGETSTART, a); send(S::SCI_SETTARGETEND, b);
  send(S::SCI_REPLACETARGET, text.size(), reinterpret_cast<Position>(text.constData()));
}
QByteArray ScintillaAccess::eol() const
{
  const auto mode = send(S::SCI_GETEOLMODE);
  return mode == S::SC_EOL_CRLF ? "\r\n" : mode == S::SC_EOL_CR ? "\r" : "\n";
}
}
