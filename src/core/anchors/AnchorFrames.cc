#include "core/anchors/AnchorFrames.h"
#include <Eigen/SVD>
#include <cmath>
#include <limits>
namespace Anchors {
namespace {
bool affine(const Transform3d& frame) {
  return frame.matrix().allFinite() && frame.matrix().row(3).isApprox(Eigen::RowVector4d(0,0,0,1),1e-10);
}
bool normalize(Vector3d& vector) {
  const double scale = vector.cwiseAbs().maxCoeff();
  if (!vector.allFinite() || scale == 0) return false;
  vector = (vector / scale).normalized(); return vector.allFinite();
}
std::optional<Matrix3d> basis(Vector3d normal, Vector3d up) {
  if (!normalize(normal) || !normalize(up)) return {};
  up -= normal * up.dot(normal);
  if (up.norm() <= 1e-10 || !normalize(up)) return {};
  Matrix3d rotation; rotation.col(0) = up.cross(normal); rotation.col(1) = up; rotation.col(2) = normal; return rotation;
}
FrameResult failure(const std::string& text) { FrameResult out;out.diagnostic=text;return out; }
}
bool properRigid(const Transform3d& frame) {
  return affine(frame) && (frame.linear().transpose()*frame.linear()-Matrix3d::Identity()).norm() <= 1e-10 &&
    std::abs(frame.linear().determinant()-1) <= 1e-10;
}
std::optional<Transform3d> supportedInverse(const Transform3d& owner,std::string& error) {
  if (!affine(owner)) {error="Owner frame must be a finite affine matrix";return {};}
  const double scale=owner.linear().cwiseAbs().maxCoeff();
  if (scale==0) {error="Owner frame is singular";return {};}
  const Matrix3d linear=owner.linear()/scale;
  Eigen::JacobiSVD<Matrix3d> decomposition(linear);
  const auto values=decomposition.singularValues();
  if (!values.allFinite() || values[0]==0 || values[2] <= 128*std::numeric_limits<double>::epsilon()*values[0]) {
    error="Owner frame is singular or outside supported conditioning";return {};
  }
  Transform3d inverse=Transform3d::Identity();inverse.linear()=linear.inverse()/scale;
  inverse.translation()=-inverse.linear()*owner.translation();
  if (!inverse.matrix().allFinite()) {error="Owner inverse exceeds finite coordinates";return {};}
  return inverse;
}
FrameResult localFrame(const Anchor& anchor) {
  auto rotation=basis(anchor.normal,anchor.up);
  const double scale=anchor.rotation.coeffs().cwiseAbs().maxCoeff();
  if (!rotation || !anchor.position.allFinite() || !anchor.translation.allFinite() || !std::isfinite(anchor.spin) ||
      !anchor.rotation.coeffs().allFinite() || scale==0) return failure("Anchor requires a finite point and complete nonparallel orientation");
  Eigen::Quaterniond adjustment=anchor.rotation;adjustment.coeffs()/=scale;adjustment.normalize();
  Transform3d frame=Transform3d::Identity();frame.translation()=anchor.position+anchor.translation;
  frame.linear()=adjustment.toRotationMatrix()*(*rotation)*Eigen::AngleAxisd(std::remainder(anchor.spin,360.0)*std::acos(-1.0)/180,Vector3d::UnitZ()).toRotationMatrix();
  if (!properRigid(frame)) return failure("Anchor frame exceeds finite/proper rigid limits");
  FrameResult out;out.state=Design::ResolutionState::Resolved;out.frame=frame;return out;
}
FrameResult transformReference(const ReferenceValue& value,const Transform3d& owner) {
  if (!affine(owner)) return failure("Owner frame must be finite and affine");
  FrameResult out;out.state=Design::ResolutionState::Resolved;
  const double scale=owner.linear().cwiseAbs().maxCoeff();
  out.reflectedOwner=scale>0 && (owner.linear()/scale).determinant()<0;
  auto point=[&](const Vector3d& position) {return Vector3d(owner*position);};
  if (auto anchor=std::get_if<Anchor>(&value)) {
    auto local=localFrame(*anchor);if(!local)return local;
    std::string error;auto inverse=supportedInverse(owner,error);if(!inverse)return failure(error);
    auto rotation=basis(inverse->linear().transpose()*local.frame->linear().col(2),owner.linear()*local.frame->linear().col(1));
    if(!rotation)return failure("Affine Anchor orientation is unavailable");
    Transform3d frame=Transform3d::Identity();frame.linear()=*rotation;frame.translation()=point(local.frame->translation());
    if(!properRigid(frame))return failure("Transformed Anchor is outside finite/proper rigid limits");
    Anchor world;world.position=frame.translation();world.normal=frame.linear().col(2);world.up=frame.linear().col(1);world.fixedCapture=anchor->fixedCapture;world.drawingFrame=anchor->drawingFrame;
    out.worldValue=world;out.frame=frame;
  } else if (auto axis=std::get_if<Axis>(&value)) {
    Axis world{point(axis->position),owner.linear()*axis->direction};
    if(!world.position.allFinite() || !normalize(world.direction))return failure("Axis point or tangent direction is unavailable");
    out.worldValue=world;
  } else if (auto plane=std::get_if<Plane>(&value)) {
    std::string error;auto inverse=supportedInverse(owner,error);if(!inverse)return failure(error);
    Plane world{point(plane->position),inverse->linear().transpose()*plane->normal};
    if(!world.position.allFinite() || !normalize(world.normal))return failure("Plane point or surface normal is unavailable");
    out.worldValue=world;
  } else {
    Point world{point(std::get<Point>(value).position)};if(!world.position.allFinite())return failure("Point exceeds finite coordinates");out.worldValue=world;
  }
  return out;
}
CaptureResult captureAnchor(const Vector3d& point,const Vector3d& normal,const Vector3d& cameraUp,const Transform3d& owner) {
  if(!point.allFinite() || !cameraUp.allFinite())return {{},"Capture requires a finite hit and camera up"};
  Vector3d n=normal;if(!normalize(n))return {{},"Capture normal is unavailable"};
  Vector3d up=cameraUp;
  if(!normalize(up) || (up-n*up.dot(n)).norm()<=1e-10) {
    int axis=0;for(int i=1;i<3;++i)if(std::abs(n[i])<std::abs(n[axis]))axis=i;
    up=Vector3d::Zero();up[axis]=1;
  }
  auto worldBasis=basis(n,up);if(!worldBasis)return {{},"Capture up is unavailable"};
  std::string error;auto inverse=supportedInverse(owner,error);if(!inverse)return {{},error};
  Anchor anchor;anchor.fixedCapture=true;anchor.position=*inverse*point;
  auto localBasis=basis(owner.linear().transpose()*n,inverse->linear()*worldBasis->col(1));
  if(!localBasis || !anchor.position.allFinite())return {{},"Owner-local capture is unavailable"};
  anchor.normal=localBasis->col(2);anchor.up=localBasis->col(1);
  return {anchor,{}};
}
}
