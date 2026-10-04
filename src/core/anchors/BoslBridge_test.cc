#include "core/anchors/AnchorOperations.h"
#include "core/semantic/SemanticTestFixture.h"
#include "core/parsersettings.h"
#include <cstdlib>
#include <sstream>
extern std::vector<std::string> librarypath;
using namespace Anchors;
namespace {
struct Libraries {
  std::vector<std::string> before=librarypath;
  Libraries() {
    const auto *root=std::getenv("SCADMAXX_PROVIDER_BOSL2_ROOT");REQUIRE(root);
    REQUIRE(std::filesystem::is_regular_file(std::filesystem::path(root)/"std.scad"));
    librarypath.push_back(std::filesystem::path(root).parent_path().generic_string());
    librarypath.push_back((std::filesystem::path(OPENSCAD_TEST_DATA_DIR).parent_path().parent_path()/"libraries").generic_string());
  }
  ~Libraries(){librarypath=std::move(before);}
};
std::string fixtureSource() {
  std::ifstream file(std::filesystem::path(OPENSCAD_TEST_DATA_DIR)/"scad/anchors/bosl2-bridge.scad",std::ios::binary);REQUIRE(file);std::ostringstream out;out<<file.rdbuf();return out.str();
}
ReferenceResolution anchor(const SemanticTest::Fixture& fixture,const std::string& id) {
  return resolveReference(fixture.scene,{{OwnerRole::Document,fixture.inputs->root().fileKey},id});
}
}
TEST_CASE("Pinned BOSL2 bridge publishes typed primitive named override and reoriented Anchor frames with captured sources", "[anchor-bosl2]") {
  Libraries libraries;SemanticTest::Fixture fixture;fixture.evaluate(fixtureSource());REQUIRE(fixture.node);
  for(const std::string id:{"top","edge","corner","cylinder-end","cylinder-radial","sphere-direction","named","affine","reoriented","overridden"}) {
    auto result=anchor(fixture,id);INFO(id);REQUIRE(result.state==Design::ResolutionState::Resolved);REQUIRE(result.frame);REQUIRE(properRigid(*result.frame));REQUIRE(result.declaration->source);
  }
  REQUIRE(anchor(fixture,"top").frame->translation().isApprox(Vector3d(0,0,5)));
  REQUIRE(anchor(fixture,"edge").frame->translation().isApprox(Vector3d(10,0,5)));
  REQUIRE(anchor(fixture,"corner").frame->translation().isApprox(Vector3d(10,15,5)));
  REQUIRE(anchor(fixture,"cylinder-end").frame->translation().isApprox(Vector3d(0,0,10)));
  REQUIRE(anchor(fixture,"cylinder-radial").frame->translation().isApprox(Vector3d(5,0,0)));
  REQUIRE(anchor(fixture,"sphere-direction").frame->translation().isApprox(Vector3d(1,2,3).normalized()*10));
  REQUIRE(anchor(fixture,"overridden").frame->translation().isApprox(Vector3d(1,2,3)));
  bool helper=false,pinned=false;for(const auto& [key,input]:fixture.inputs->consumed()) {
    helper|=key.find("bosl2-anchors.scad")!=std::string::npos;pinned|=key.find("BOSL2/attachments.scad")!=std::string::npos;
  }REQUIRE(helper);REQUIRE(pinned);
}
TEST_CASE("Implemented BOSL2 bridge agrees with C++ affine normals up and rigid attachment solve", "[anchor-bosl2]") {
  Libraries libraries;SemanticTest::Fixture fixture;fixture.evaluate(fixtureSource());REQUIRE(fixture.node);
  auto named=anchor(fixture,"named");Anchor base=std::get<Anchor>(*named.local);
  Transform3d placement=Transform3d::Identity();placement.translation()=Vector3d(5,8,3);
  placement.linear()=(Eigen::AngleAxisd(56*std::acos(-1.0)/180,Vector3d::UnitZ())*Eigen::AngleAxisd(34*std::acos(-1.0)/180,Vector3d::UnitY())*Eigen::AngleAxisd(12*std::acos(-1.0)/180,Vector3d::UnitX())).toRotationMatrix()*Vector3d(-2,3,4).asDiagonal();
  auto expected=transformReference(base,placement);REQUIRE(expected);REQUIRE(anchor(fixture,"affine").frame->matrix().isApprox(expected.frame->matrix(),1e-10));
  AttachmentDeclaration relation;relation.offset=Vector3d(0,0,2);relation.spin=15;
  auto solved=solvePlacement(*anchor(fixture,"child-named").frame,*anchor(fixture,"parent-named").frame,relation.mode,relation.offset,relation.spin);REQUIRE(solved);
  REQUIRE((solved.correction->matrix()).isApprox(anchor(fixture,"expected-child").frame->matrix(),1e-10));
}
