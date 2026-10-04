#include "core/design/DesignSourceMap.h"

namespace Design {
std::optional<DeclarationKey> declarationKey(const Location& location, const SourceInput& input, const std::string& name) {
  if (location.isNone()) return {};
  std::vector<size_t> lines{0};
  for (size_t i = 0; i < input.utf8.size(); ++i) if (input.utf8[i] == '\n') lines.push_back(i + 1);
  auto byte = [&](int line, int column) -> std::optional<size_t> {
    if (line < 1 || column < 1 || size_t(line) > lines.size()) return {};
    size_t pos = lines[line - 1] + column - 1;
    if (pos > input.utf8.size() || (size_t(line) < lines.size() && pos >= lines[line])) return {};
    return pos;
  };
  auto begin = byte(location.firstLine(), location.firstColumn()), end = byte(location.lastLine(), location.lastColumn());
  if (!begin || !end || *begin >= *end) return {};
  return DeclarationKey{input.fileKey, input.fingerprint, name, DeclarationHost::Assignment, *begin, *end};
}
}
