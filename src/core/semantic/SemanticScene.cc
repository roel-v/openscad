#include "core/semantic/SemanticScene.h"
namespace Semantic {
std::string escapeSegment(const std::string& value) {
  std::string result;
  for (char c : value) result += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
  return result;
}
std::optional<std::vector<std::string>> pathSegments(const std::string& path) {
  if (path.empty()) return std::vector<std::string>{};
  if (path.front() != '/') return {};
  std::vector<std::string> result; std::string segment;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (segment.empty()) return {};
      result.push_back(std::move(segment)); segment.clear();
    } else if (path[i] == '~') {
      if (++i == path.size() || (path[i] != '0' && path[i] != '1')) return {};
      segment += path[i] == '0' ? '~' : '/';
    } else segment += path[i];
  }
  return result;
}
}
