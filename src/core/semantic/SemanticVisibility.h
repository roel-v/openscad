#pragma once
#include "core/semantic/SemanticScene.h"
namespace Semantic {
bool descendantOf(const std::string& path, const std::string& ancestor);
struct Visibility {
  std::set<std::string> hidden;
  std::optional<std::string> solo;
  bool otherVisible = true;
  bool visible(const std::string& path) const;
  void prune(const Scene&);
};
}
