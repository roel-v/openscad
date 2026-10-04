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
#include "gui/vim/engine/ScintillaAccess.h"
#include <QString>
#include <vector>
#include <functional>
#include <optional>

namespace Vim {
enum class Mode { Normal, Insert, VisualChar, VisualLine };
enum class InputResult { Consumed, PassToEditor, PassToApplication };
enum class HostCommand { Save, Close, Reload, Open };
enum class HostResult { Success, Cancelled, Failed };
struct HostRequest { HostCommand command; QString path; };
struct Range { Position begin = 0, end = 0; bool linewise = false, valid = false; };
struct InsertEdit { Position offset = 0; QByteArray removed, inserted; };
struct Repeat {
  QString command;
  std::vector<InsertEdit> journal;
  bool valid = false;
};
struct Register { QByteArray text; bool linewise = false; };
Register& unnamedRegister();
struct Context {
  Mode mode = Mode::Normal;
  bool enabled = false, undoOpen = false, ownedEdit = false, recording = false;
  unsigned inputDepth = 0;
  QString keys, searchText, findText, feedback;
  bool rejectedCount = false;
  QString rejectedKeys;
  std::optional<InsertEdit> pendingInsertion;
  Position undoPosition = -1;
  QChar findKind;
  bool searchForward = true;
  Position anchor = 0, caret = 0, desiredColumn = -1, insertOrigin = 0;
  Position defaultCaret = 1, defaultPeriod = 500;
  Repeat repeat, insertion;
  std::uint64_t insertCount = 1;
  qsizetype journalBytes = 0;
  std::function<void()> changed;
  std::function<void(QChar)> prompt;
  std::function<HostResult(const HostRequest&)> host;
};
constexpr qsizetype MaxGeneratedBytes = 64 * 1024 * 1024;
}
