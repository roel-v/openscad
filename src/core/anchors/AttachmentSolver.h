#pragma once
#include "core/anchors/AnchorFrames.h"
#include "core/anchors/AttachmentGraph.h"
namespace Anchors {
struct PlacementResult {
  std::optional<Transform3d> correction;
  std::string diagnostic;
  explicit operator bool() const {return bool(correction);}
};
PlacementResult solvePlacement(const Transform3d& childFrame,const Transform3d& targetFrame,
                               AttachmentMode,const Vector3d& offset,double spin);
struct SolvedGraph {
  GraphResult graph;
  std::map<std::string,Transform3d> placements,worldCorrections;
  std::vector<Design::Diagnostic> diagnostics;
  bool valid() const {return graph.valid() && diagnostics.empty();}
};
SolvedGraph solveGraph(const ReferenceScene&,const std::map<std::string,Semantic::Instance>&);
}
