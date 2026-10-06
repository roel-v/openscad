#include "core/anchors/AttachmentSolver.h"
#include <cmath>
namespace Anchors {
PlacementResult solvePlacement(const Transform3d& child,const Transform3d& target,AttachmentMode mode,const Vector3d& offset,double spin) {
  if(!properRigid(child) || !properRigid(target) || !offset.allFinite() || !std::isfinite(spin))return {{},"Placement needs finite proper rigid Anchor frames, offset and spin"};
  Transform3d desired=target;desired.translate(offset);
  desired.rotate(Eigen::AngleAxisd(std::remainder(spin,360.0)*std::acos(-1.0)/180,Vector3d::UnitZ()));
  if(mode==AttachmentMode::Mate)desired.rotate(Eigen::AngleAxisd(std::acos(-1.0),Vector3d::UnitY()));
  Transform3d correction=desired*child.inverse();
  if(!properRigid(correction))return {{},"World placement correction is outside finite/proper rigid limits"};
  return {correction,{}};
}
SolvedGraph solveGraph(const ReferenceScene& scene,const std::map<std::string,Semantic::Instance>& instances) {
  SolvedGraph out;out.graph=attachmentGraph(instances,scene.attachments);if(!out.graph.valid())return out;
  out.placements[""]=Transform3d::Identity();
  for(const auto& path:out.graph.order) {
    if(path.empty())continue;
    const auto& instance=instances.at(path);
    Transform3d base=out.placements.at(instance.parentPath)*instance.originalLocalToParent;
    auto driver=out.graph.drivers.find(path);
    if(driver==out.graph.drivers.end()){out.placements[path]=base;continue;}
    const auto& relation=scene.attachments.at(driver->second);
    auto fail=[&](const std::string& text) {out.diagnostics.push_back({"attachment-solve",path+": "+text,relation.declaration ? relation.declaration->source : std::nullopt});};
    auto child=scene.references.find(relation.child.reference),target=scene.references.find(relation.target.reference);
    if(child==scene.references.end() || target==scene.references.end()) {fail("Missing named attachment reference");return out;}
    auto childFrame=transformReference(child->second.value,base);
    auto targetFrame=transformReference(target->second.value,out.placements.at(relation.target.path));
    if(!childFrame || !targetFrame || !childFrame.frame || !targetFrame.frame) {fail(!childFrame ? childFrame.diagnostic : !targetFrame ? targetFrame.diagnostic : "Both endpoints require oriented Anchors");return out;}
    auto solved=solvePlacement(*childFrame.frame,*targetFrame.frame,relation.value.mode,relation.value.offset+relation.value.offsetAdjustment,relation.value.spin+relation.value.spinAdjustment);
    if(!solved){fail(solved.diagnostic);return out;}
    Transform3d placement=*solved.correction*base;
    if(!placement.matrix().allFinite()){fail("Solved placement exceeds finite coordinates");return out;}
    out.placements[path]=placement;out.worldCorrections[path]=*solved.correction;
  }
  return out;
}
}
