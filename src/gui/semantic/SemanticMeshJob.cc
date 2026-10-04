#include "gui/semantic/SemanticMeshJob.h"
#include "core/NativeExecutionGuard.h"
#include "core/Tree.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySetUtils.h"
namespace Semantic {
MeshOutput prepareMeshes(const Scene& scene,std::string resultId,uint64_t generation,const std::function<bool()>& cancelled) {
  MeshOutput output; auto stopped=[&]{return cancelled && cancelled();};
  if(!scene.marked || !scene.valid()){output.error="A valid semantic scene is required";return output;}
  NativeExecutionGuard execution;
  auto set=std::make_shared<InspectionMeshSet>();set->resultId=std::move(resultId);set->geometryGeneration=generation;
  try {
    auto append=[&](const Piece& piece) {
      if(stopped()){output.cancelled=true;return false;}
      Tree tree(piece.root,scene.documentPath);GeometryEvaluator evaluator(tree);
      auto geometry=evaluator.evaluateGeometry(*piece.root,true);if(!geometry || geometry->isEmpty())return true;
      if(geometry->getDimension()!=3){output.error="Semantic inspection requires convertible 3D geometry";return false;}
      if(piece.inheritedColor.isValid()) {auto colored=std::shared_ptr<Geometry>(geometry->copy());colored->setColor(piece.inheritedColor);geometry=std::move(colored);}
      auto mesh=PolySetUtils::getGeometryAsPolySet(geometry);
      if(!mesh){output.error="Semantic geometry cannot be converted for inspection";return false;}
      if(mesh->isEmpty())return true;
      if(stopped()){output.cancelled=true;return false;}
      const uint64_t id=set->pieces.size()+1;
      set->pieces.push_back({id,static_cast<size_t>(piece.root->index()),mesh,piece.localToWorld,Color4f(),
        piece.background?InspectionPiece::Role::Background:piece.highlight?InspectionPiece::Role::Highlight:InspectionPiece::Role::Solid});
      output.paths.emplace(id,piece.instancePath);return true;
    };
    for(const auto& piece:scene.parts)if(!append(piece))return output;
    for(const auto& piece:scene.residual)if(!append(piece))return output;
    if(stopped()){output.cancelled=true;return output;}
    output.meshes=std::move(set);
  }catch(const std::exception& error){output.error=error.what();}
  return output;
}
}
