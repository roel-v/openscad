#pragma once
#include "core/semantic/SemanticScene.h"
#include "glview/InspectionPresentation.h"
#include <functional>
namespace Semantic {
struct MeshOutput {
  std::shared_ptr<const InspectionMeshSet> meshes;
  std::map<uint64_t,std::string> paths;
  std::string error;
  bool cancelled=false;
};
MeshOutput prepareMeshes(const Scene&, std::string resultId, uint64_t generation,
                         const std::function<bool()>& cancelled={});
}
