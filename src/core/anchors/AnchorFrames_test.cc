#include "core/anchors/AnchorFrames.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <limits>
#include <algorithm>
#include <cmath>
#include <random>
using namespace Anchors;
namespace {
void close(const Vector3d& a,const Vector3d& b,double tolerance=1e-9) {REQUIRE((a-b).norm()<=tolerance*std::max({1.0,a.norm(),b.norm()}));}
Transform3d owner(double scale,bool mirror) {
  Transform3d out=Transform3d::Identity();Matrix3d stretch;stretch<<2,0.3,0.1,0,3,0.2,0,0,4;if(mirror)stretch.col(0)*=-1;
  out.linear()=Eigen::AngleAxisd(0.7,Vector3d(1,2,3).normalized()).toRotationMatrix()*stretch*scale;out.translation()=Vector3d(12,-20,30)*scale;return out;
}
}
TEST_CASE("Anchor local spin and owner-coordinate quaternion adjustment preserve the anchor point", "[anchor-frames]") {
  Anchor anchor;anchor.position=Vector3d(2,3,4);anchor.translation=Vector3d(1,-2,5);anchor.spin=450;
  anchor.rotation=Eigen::Quaterniond(Eigen::AngleAxisd(0.6,Vector3d::UnitX()));
  auto result=localFrame(anchor);REQUIRE(result);close(result.frame->translation(),Vector3d(3,1,9));
  Matrix3d expected=anchor.rotation.toRotationMatrix()*Eigen::AngleAxisd(std::acos(-1.0)/2,Vector3d::UnitZ()).toRotationMatrix();
  REQUIRE(result.frame->linear().isApprox(expected,1e-12));
  for(double spin:{-1e200,1e200,-720.0,0.0,720.0}){anchor.spin=spin;REQUIRE(localFrame(anchor));}
  anchor.spin=std::numeric_limits<double>::infinity();REQUIRE_FALSE(localFrame(anchor));
  anchor.spin=0;anchor.normal=Vector3d::Zero();REQUIRE_FALSE(localFrame(anchor));
  anchor.normal=Vector3d::UnitY();REQUIRE_FALSE(localFrame(anchor));
}
TEST_CASE("Affine Anchor surface normals up and parity remain proper under scale mirror and shear", "[anchor-frames]") {
  Anchor anchor;anchor.position=Vector3d(2,3,4);anchor.normal=Vector3d(1,2,3);anchor.up=Vector3d(-1,2,0);anchor.spin=37;
  auto local=localFrame(anchor);REQUIRE(local);
  for(double scale:{1e-6,1.0,1e6})for(bool mirror:{false,true}) {
    const auto transform=owner(scale,mirror);auto world=transformReference(anchor,transform);REQUIRE(world);REQUIRE(world.frame);REQUIRE(properRigid(*world.frame));REQUIRE(world.reflectedOwner==mirror);
    close(world.frame->translation(),transform*anchor.position);
    const Vector3d n=world.frame->linear().col(2);
    for(int i:{0,1}) {Vector3d tangent=(transform.linear()*local.frame->linear().col(i)).normalized();REQUIRE(std::abs(n.dot(tangent))<1e-12);}
    auto captured=captureAnchor(world.frame->translation(),n,world.frame->linear().col(1),transform);REQUIRE(captured);REQUIRE(captured.anchor->fixedCapture);
    auto roundtrip=transformReference(*captured.anchor,transform);REQUIRE(roundtrip);REQUIRE(roundtrip.frame->matrix().isApprox(world.frame->matrix(),1e-10));
  }
}
TEST_CASE("Typed affine references distinguish points tangent directions and surface normals", "[anchor-frames]") {
  auto transform=owner(1,true);
  auto axis=transformReference(Axis{Vector3d(1,2,3),Vector3d(1,1,1)},transform);
  auto plane=transformReference(Plane{Vector3d(1,2,3),Vector3d(1,1,1)},transform);
  auto point=transformReference(Point{Vector3d(1,2,3)},transform);
  REQUIRE(axis);REQUIRE(plane);REQUIRE(point);REQUIRE_FALSE(axis.frame);REQUIRE_FALSE(plane.frame);REQUIRE_FALSE(point.frame);
  close(std::get<Axis>(*axis.worldValue).direction,(transform.linear()*Vector3d(1,1,1)).normalized());
  close(std::get<Plane>(*plane.worldValue).normal,(transform.linear().inverse().transpose()*Vector3d(1,1,1)).normalized());
  REQUIRE_FALSE(std::get<Axis>(*axis.worldValue).direction.isApprox(std::get<Plane>(*plane.worldValue).normal,1e-6));
}
TEST_CASE("Required owner inverses reject singular and unit-independent ill conditioning", "[anchor-frames]") {
  for(double scale:{1e-6,1.0,1e6}) {
    Transform3d singular=Transform3d::Identity();singular.linear()=Vector3d(scale,scale,0).asDiagonal();std::string error;
    REQUIRE_FALSE(supportedInverse(singular,error));REQUIRE_FALSE(transformReference(Anchor{},singular));REQUIRE_FALSE(transformReference(Plane{},singular));
    REQUIRE(transformReference(Point{Vector3d(1,2,3)},singular));REQUIRE_FALSE(transformReference(Axis{},singular));
    singular.linear()(2,2)=scale*1e-14;REQUIRE_FALSE(supportedInverse(singular,error));
    singular.linear()(2,2)=scale*1e-12;REQUIRE(supportedInverse(singular,error));
  }
  Transform3d invalid=Transform3d::Identity();invalid.linear()(0,0)=std::numeric_limits<double>::quiet_NaN();REQUIRE_FALSE(transformReference(Point{},invalid));
}
TEST_CASE("Capture chooses a deterministic tangent up and retains it after camera movement", "[anchor-frames]") {
  auto captured=captureAnchor(Vector3d(1,2,3),Vector3d::UnitZ(),Vector3d::UnitZ(),Transform3d::Identity());REQUIRE(captured);
  REQUIRE(captured.anchor->up.isApprox(Vector3d::UnitX()));auto saved=localFrame(*captured.anchor);REQUIRE(saved);
  auto otherCamera=captureAnchor(Vector3d(1,2,3),Vector3d::UnitZ(),Vector3d::UnitY(),Transform3d::Identity());REQUIRE(otherCamera);
  REQUIRE_FALSE(otherCamera.anchor->up.isApprox(captured.anchor->up));REQUIRE(localFrame(*captured.anchor).frame->matrix().isApprox(saved.frame->matrix()));
}
TEST_CASE("Fixed-seed rigid owner transformations roundtrip arbitrary captured Anchor frames", "[anchor-frames]") {
  std::mt19937 random(0x0804);std::uniform_real_distribution<double> number(-1,1);
  for(int i=0;i<40;++i) {
    Anchor anchor;anchor.position=Vector3d(number(random),number(random),number(random))*100;
    anchor.normal=Vector3d(number(random),number(random),number(random));anchor.up=Vector3d(number(random),number(random),number(random));anchor.spin=number(random)*720;
    Transform3d transform=Transform3d::Identity();transform.linear()=Eigen::AngleAxisd(number(random)*3,Vector3d(number(random),number(random),number(random)).normalized()).toRotationMatrix();transform.translation()=Vector3d(number(random),number(random),number(random))*100;
    auto world=transformReference(anchor,transform);REQUIRE(world);auto captured=captureAnchor(world.frame->translation(),world.frame->linear().col(2),world.frame->linear().col(1),transform);REQUIRE(captured);
    auto roundtrip=transformReference(*captured.anchor,transform);REQUIRE(roundtrip);REQUIRE(roundtrip.frame->matrix().isApprox(world.frame->matrix(),1e-10));
  }
}
