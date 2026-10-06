#include "core/semantic/SemanticVisibility.h"
namespace Semantic {
bool descendantOf(const std::string& path, const std::string& ancestor) {
  return path == ancestor || (path.size() > ancestor.size() && path.compare(0,ancestor.size(),ancestor)==0 && path[ancestor.size()]=='/');
}
bool Visibility::visible(const std::string& path) const {
  if (solo) return descendantOf(path,*solo);
  for (const auto& ancestor : hidden) if (descendantOf(path,ancestor)) return false;
  return true;
}
void Visibility::prune(const Scene& scene) {
  for (auto it=hidden.begin();it!=hidden.end();) if (!scene.instances.count(*it)) it=hidden.erase(it); else ++it;
  if (solo && !scene.instances.count(*solo)) solo.reset();
}
}
