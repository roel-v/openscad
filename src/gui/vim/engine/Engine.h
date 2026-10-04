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
#include "gui/vim/engine/CommandRunner.h"
#include "gui/vim/engine/QtKey.h"

namespace Vim {
class Engine {
  ScintillaAccess sci;
  Context context;
  bool execute(const Command&, bool repeat = false);
  bool repeat(std::uint64_t);
  bool repeatInsertionCount();
  void enterInsert(const QString&, Position, std::uint64_t count = 1);
  void finishInsert(bool saveRepeat, bool moveCaret);
  void visualSelection();
  Range selection() const;
  Range resolve(const Command&);
  void status(const QString&);
public:
  explicit Engine(QsciScintilla *);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  InputResult key(const Key&);
  bool claimsShortcut(const Key&) const;
  void cancelPending();
  void beforeExternalMutation();
  void setEnabled(bool);
  bool enabled() const { return context.enabled; }
  Mode mode() const { return context.mode; }
  const QString& feedback() const { return context.feedback; }
  void callbacks(std::function<void()>, std::function<void(QChar)>, std::function<HostResult(const HostRequest&)> = {});
  void beginNativeInput() { ++context.inputDepth; }
  void endNativeInput() { --context.inputDepth; }
  void modified(Position, int type, const char *, Position length);
  void submitSearch(const QString&, bool forward);
  void submitCommand(const QString&);
  Position position() const { return context.mode == Mode::VisualChar || context.mode == Mode::VisualLine ? context.caret : sci.pos(); }
  bool undoGroupOpen() const { return context.undoOpen; }
};
}
