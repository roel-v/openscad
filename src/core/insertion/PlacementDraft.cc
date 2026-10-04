// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/PlacementDraft.h"
#include <cmath>
namespace Insertion {
namespace {
template<class T> Result<T> fail(std::string message){return {{},Diagnostic{"UnsupportedPlacement",std::move(message),{}}};}
Anchors::Anchor contact(const Providers::LocalFrame& frame){Anchors::Anchor out;out.position={frame.origin[0],frame.origin[1],frame.origin[2]};out.normal={frame.normal[0],frame.normal[1],frame.normal[2]};out.up={frame.up[0],frame.up[1],frame.up[2]};return out;}
Result<Transform3d> targetWorld(const PlacementDraft& draft,std::shared_ptr<const Semantic::Scene> scene){
  if(draft.mode==PlacementDraft::Mode::NamedAttachment){if(!scene || !draft.namedTarget)return fail<Transform3d>("Choose a current named Anchor");auto resolved=Anchors::resolveReference(scene,*draft.namedTarget);if(resolved.state!=Design::ResolutionState::Resolved || !resolved.frame || !resolved.local || !std::holds_alternative<Anchors::Anchor>(*resolved.local))return fail<Transform3d>(resolved.diagnostic.empty()?"The selected reference is not an oriented Anchor":resolved.diagnostic);return {*resolved.frame,{}};}
  Transform3d owner=Transform3d::Identity();if(!draft.fixedOwnerPath.empty()){if(!scene)return fail<Transform3d>("The captured owner is unavailable");auto found=scene->instances.find(draft.fixedOwnerPath);if(found==scene->instances.end())return fail<Transform3d>("The captured owner is unavailable");owner=found->second.localToWorld;}
  auto transformed=Anchors::transformReference(draft.fixedTarget,owner);if(!transformed || !transformed.frame)return fail<Transform3d>(transformed.diagnostic);return {*transformed.frame,{}};
}
}
Result<PlacementDraft> PlacementDraft::capture(const SourceTarget& target,const Providers::GeneratedFragment& fragment,
  const Vector3d& point,const Vector3d& normal,const Vector3d& up,std::shared_ptr<const Semantic::Scene> scene,const std::string& ownerPath){
  PlacementDraft draft;Transform3d owner=target.localToWorld;
  if(fragment.insertionRole==Providers::InsertionRole::Component){draft.mode=Mode::FixedComponent;draft.fixedOwnerPath=ownerPath;owner=Transform3d::Identity();
    if(!ownerPath.empty()){if(!scene || !scene->instances.count(ownerPath))return fail<PlacementDraft>("The picked semantic owner is unavailable");owner=scene->instances.at(ownerPath).localToWorld;}}
  auto captured=Anchors::captureAnchor(point,normal,up,owner);if(!captured)return fail<PlacementDraft>(captured.diagnostic);draft.fixedTarget=*captured.anchor;return {std::move(draft),{}};
}
Result<Transform3d> PlacementDraft::localPlacement(const Providers::GeneratedFragment& fragment) const {
  if(mode!=Mode::FixedGeometry)return fail<Transform3d>("Local geometry placement requires a fixed source frame");auto child=Anchors::localFrame(contact(fragment.frame)),target=Anchors::localFrame(fixedTarget);if(!child || !target)return fail<Transform3d>(!child?child.diagnostic:target.diagnostic);
  auto solved=Anchors::solvePlacement(*child.frame,*target.frame,attachmentMode(),offset,spinDegrees);if(!solved)return fail<Transform3d>(solved.diagnostic);return {*solved.correction,{}};
}
Result<Transform3d> PlacementDraft::worldPlacement(const SourceTarget& target,const Providers::GeneratedFragment& fragment,std::shared_ptr<const Semantic::Scene> scene) const {
  if(mode==Mode::FixedGeometry){auto local=localPlacement(fragment);if(!local)return local;return {Transform3d(target.localToWorld*(*local.value)),{}};}
  auto frame=targetWorld(*this,scene);if(!frame)return frame;auto child=Anchors::transformReference(contact(fragment.frame),target.localToWorld);if(!child || !child.frame)return fail<Transform3d>(child.diagnostic);auto solved=Anchors::solvePlacement(*child.frame,*frame.value,attachmentMode(),offset,spinDegrees);if(!solved)return fail<Transform3d>(solved.diagnostic);return {Transform3d(*solved.correction*target.localToWorld),{}};
}
Result<Transform3d> PlacementDraft::movementFrame(const SourceTarget& target,std::shared_ptr<const Semantic::Scene> scene) const {
  if(mode!=Mode::FixedGeometry)return targetWorld(*this,std::move(scene));auto local=Anchors::localFrame(fixedTarget);if(!local)return fail<Transform3d>(local.diagnostic);return {Transform3d(target.localToWorld*(*local.frame)),{}};
}
Result<Vector2d> PlacementDraft::planeCoordinates(const SourceTarget& target,std::shared_ptr<const Semantic::Scene> scene,const Vector3d& origin,const Vector3d& direction) const {
  if(!origin.allFinite() || !direction.allFinite() || direction.norm()==0)return fail<Vector2d>("The camera ray is unavailable");auto frame=movementFrame(target,std::move(scene));if(!frame)return {{},frame.diagnostic};std::string error;auto inverse=Anchors::supportedInverse(*frame.value,error);if(!inverse)return fail<Vector2d>(error);
  const Vector3d normal=(inverse->linear().transpose()*Vector3d::UnitZ()).normalized();const double denominator=normal.dot(direction);if(std::abs(denominator)<=1e-10*direction.norm())return fail<Vector2d>("Camera ray is parallel to the placement plane");const double distance=normal.dot(frame.value->translation()-origin)/denominator;if(distance<0 || !std::isfinite(distance))return fail<Vector2d>("Placement plane is behind the camera");const Vector3d local=*inverse*(origin+distance*direction);return {Vector2d(local.x(),local.y()),{}};
}
std::optional<double> PlacementDraft::spinAngle(const Vector2d& point,const Vector2d& origin){auto relative=point-origin;if(!relative.allFinite() || relative.norm()<=1e-8)return {};return std::atan2(relative.y(),relative.x())*180/std::acos(-1.0);}
}
