#pragma once
#include "glview/Renderer.h"
#include "geometry/PolySet.h"
#include "core/CSGNode.h"
#include <unordered_set>

// Match VBOBuilder's prepared surface tessellation: triangles, split quads, then center fans.
// These are submitted model triangles, not visible pixels or repeated GPU passes.
inline std::optional<uint64_t> preparedTriangleCount(const PolySet& polyset)
{
  uint64_t count=0;
  for (const auto& face:polyset.indices) {
    if (face.size()<3) return std::nullopt;
    count+=face.size()==3 ? 1 : face.size()==4 ? 2 : face.size();
  }
  return count;
}
inline Renderer::TriangleStatistics previewTriangleStatistics(
  const std::shared_ptr<CSGProducts>& root,const std::shared_ptr<CSGProducts>& highlights,
  const std::shared_ptr<CSGProducts>& background)
{
  uint64_t count=0;
  std::unordered_set<const CSGLeaf *> seen;
  for (const auto& group:{root,highlights,background}) if (group) {
    for (const auto& product:group->products) {
      for (const auto* chain:{&product.intersections,&product.subtractions}) for (const auto& object:*chain) {
        const auto leaf=object.leaf.get();
        if (leaf && leaf->polyset && seen.insert(leaf).second) {
          const auto triangles=preparedTriangleCount(*leaf->polyset);
          if (!triangles) return {Renderer::TriangleStatistics::Kind::Preview,std::nullopt};
          count+=*triangles;
        }
      }
    }
  }
  return {Renderer::TriangleStatistics::Kind::Preview,count};
}
