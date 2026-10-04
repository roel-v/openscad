#include "core/anchors/AttachmentTestFixture.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <limits>
#include <cmath>
using namespace Anchors;
using namespace AnchorTest;
TEST_CASE("Align Mate spin and target offsets use explicit right-handed frame multiplication", "[attachment-solve]") {
  auto identity=Transform3d::Identity();auto aligned=solvePlacement(identity,identity,AttachmentMode::Align,Vector3d::Zero(),0);REQUIRE(aligned);REQUIRE(aligned.correction->matrix().isIdentity(1e-12));
  auto mate=solvePlacement(identity,identity,AttachmentMode::Mate,Vector3d::Zero(),0);REQUIRE(mate);REQUIRE((mate.correction->linear()*Vector3d::UnitZ()).isApprox(-Vector3d::UnitZ()));REQUIRE((mate.correction->linear()*Vector3d::UnitY()).isApprox(Vector3d::UnitY()));
  for(double spin:{0.0,90.0,-90.0,450.0}) {
    Transform3d target=Transform3d::Identity();target.rotate(Eigen::AngleAxisd(0.7,Vector3d(1,2,3).normalized()));target.translation()=Vector3d(10,20,30);
    auto solution=solvePlacement(identity,target,AttachmentMode::Align,Vector3d(2,3,4),spin);REQUIRE(solution);REQUIRE(solution.correction->translation().isApprox(target*Vector3d(2,3,4),1e-12));
    Matrix3d expected=target.linear()*Eigen::AngleAxisd(spin*std::acos(-1.0)/180,Vector3d::UnitZ()).toRotationMatrix();REQUIRE(solution.correction->linear().isApprox(expected,1e-12));
  }
}
TEST_CASE("World-rigid attachment corrections preserve transformed child distances under scale mirror and shear", "[attachment-solve]") {
  for(bool mirror:{false,true}) {
    Transform3d base=Transform3d::Identity();base.linear()<<2,0.5,0,0,3,0.1,0,0,4;if(mirror)base.linear().col(0)*=-1;base.translation()=Vector3d(30,40,50);
    Anchor anchor;anchor.position=Vector3d(1,2,3);anchor.normal=Vector3d(1,1,1);anchor.up=Vector3d(0,1,0);
    auto child=transformReference(anchor,base);REQUIRE(child);Transform3d target=Transform3d::Identity();target.translation()=Vector3d(5,6,7);
    auto solution=solvePlacement(*child.frame,target,AttachmentMode::Mate,Vector3d(0,0,2),37);REQUIRE(solution);REQUIRE(properRigid(*solution.correction));
    const Vector3d a(1,2,3),b(-4,2,8);const Transform3d placed=*solution.correction*base;
    REQUIRE((placed*a-placed*b).norm()==Catch::Approx((base*a-base*b).norm()).epsilon(1e-12));
    auto final=transformReference(anchor,placed);REQUIRE(final);REQUIRE(final.frame->translation().isApprox(Vector3d(5,6,9),1e-10));
  }
}
TEST_CASE("Directed chains solve from finalized targets and source-authored containment without accumulating", "[attachment-solve]") {
  auto native=instances({{"/plate",""},{"/peg",""},{"/tip",""}});ReferenceScene scene;
  Anchor plate;plate.position=Vector3d(20,0,10);reference(scene,"/plate",plate);Anchor peg;peg.normal=-Vector3d::UnitZ();reference(scene,"/peg",peg);reference(scene,"/tip",peg);
  add(scene,relation("peg","/peg","/plate"));auto tip=relation("tip","/tip","/peg");tip.value.mode=AttachmentMode::Align;tip.value.offset=Vector3d(0,0,12);add(scene,std::move(tip));
  auto solved=solveGraph(scene,native);REQUIRE(solved.valid());REQUIRE(solved.placements.at("/peg").translation().isApprox(Vector3d(20,0,10),1e-12));
  REQUIRE(solved.placements.at("/tip").translation().isApprox(Vector3d(20,0,-2),1e-12));
  auto again=solveGraph(scene,native);REQUIRE(again.valid());for(const auto& [path,placement]:solved.placements)REQUIRE(again.placements.at(path).matrix().isApprox(placement.matrix()));
}
TEST_CASE("Attached Assemblies carry descendants before each descendant solves its own relation", "[attachment-solve]") {
  auto native=instances({{"/assembly",""},{"/assembly/child","/assembly"},{"/target",""}});native.at("/assembly/child").originalLocalToParent.translate(Vector3d(0,5,0));ReferenceScene scene;
  reference(scene,"/assembly");reference(scene,"/assembly/child");Anchor target;target.position=Vector3d(100,0,0);reference(scene,"/target",target);
  auto parent=relation("parent","/assembly","/target");parent.value.mode=AttachmentMode::Align;add(scene,parent);
  auto solved=solveGraph(scene,native);REQUIRE(solved.valid());REQUIRE(solved.placements.at("/assembly/child").translation().isApprox(Vector3d(100,5,0)));
  auto child=relation("child","/assembly/child","/assembly");child.value.mode=AttachmentMode::Align;child.value.offset=Vector3d(0,0,8);add(scene,child);
  solved=solveGraph(scene,native);REQUIRE(solved.valid());REQUIRE(solved.placements.at("/assembly/child").translation().isApprox(Vector3d(100,0,8)));
}
TEST_CASE("Missing wrong-kind singular and nonfinite attachment endpoints return no candidate placement", "[attachment-solve]") {
  auto native=instances({{"/child",""},{"/target",""}});ReferenceScene scene;reference(scene,"/child");reference(scene,"/target");add(scene,relation("a","/child","/target"));
  SECTION("missing") {scene.references.erase(key("/target"));}
  SECTION("wrong kind") {scene.references.at(key("/target")).value=Point{};}
  SECTION("singular") {native.at("/child").originalLocalToParent.linear()(2,2)=0;}
  SECTION("nonfinite") {scene.attachments.begin()->second.value.offset.x()=std::numeric_limits<double>::infinity();}
  auto solved=solveGraph(scene,native);REQUIRE_FALSE(solved.valid());REQUIRE(solved.worldCorrections.empty());REQUIRE_FALSE(solved.placements.count("/child"));
}
