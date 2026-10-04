#include "core/anchors/AnchorResolver.h"
#include "core/semantic/SemanticTestFixture.h"
using namespace Anchors;
namespace {
std::string source(const std::string& label="Mount",const std::string& extra="") {
  return "@{type:Part,id:\"plate\"}module plate(width=40){@{type:Anchor} mount={id:\"mount\",label:\""+label+"\",position:[width/2,0,10],normal:[0,0,1],up:[0,1,0]"+extra+"};@{type:Axis} axis={id:\"axis\",position:[0,0,0],direction:[1,0,0]};cube([width,20,10]);}translate([100,0,0])@{type:Component,id:\"plate\"}plate(width=60);";
}
ReferenceKey key(const std::string& id="mount") {return {instanceOwner("/plate",""),id};}
}
TEST_CASE("Named anchor resolution retains owned local and world values while labels do not determine identity", "[anchor-resolution]") {
  std::shared_ptr<const Semantic::Scene> retained;
  {SemanticTest::Fixture fixture;fixture.evaluate(source());REQUIRE(fixture.node);retained=fixture.scene;}
  auto resolved=resolveReference(retained,key());REQUIRE(resolved.state==Design::ResolutionState::Resolved);REQUIRE(resolved.frame);REQUIRE(resolved.frame->translation().isApprox(Vector3d(130,0,10)));
  REQUIRE(std::get<Anchor>(*resolved.local).position.isApprox(Vector3d(30,0,10)));REQUIRE(resolved.declaration->label=="Mount");
  SemanticTest::Fixture renamed;renamed.evaluate(source("Changed label"));REQUIRE(renamed.node);auto same=resolveReference(renamed.scene,key());REQUIRE(same.frame->matrix().isApprox(resolved.frame->matrix()));REQUIRE(same.declaration->label=="Changed label");
  REQUIRE(resolveReference(retained,{instanceOwner("/deleted",""),"mount"}).state==Design::ResolutionState::Missing);
  auto axis=resolveReference(retained,key("axis"));REQUIRE(axis.state==Design::ResolutionState::Resolved);REQUIRE_FALSE(axis.frame);REQUIRE_FALSE(axis.bounds);
}
TEST_CASE("Registered anchor domain enforces exact result input configuration and token ownership", "[anchor-resolution]") {
  SemanticTest::Fixture fixture;fixture.evaluate(source());REQUIRE(fixture.node);auto references=std::make_shared<References>("result",fixture.scene,"inputs","config");publishReferences(references);
  auto token=anchorToken(key());REQUIRE(token);Design::Records records;Design::ResolveContext context{"result","inputs","config",BoundingBox(),records,{}};
  auto resolved=Design::resolve(*token,context);REQUIRE(resolved.state==Design::ResolutionState::Resolved);REQUIRE(resolved.frame->translation().isApprox(Vector3d(130,0,10)));
  context.evaluatedInputsFingerprint="changed";REQUIRE(Design::resolve(*token,context).state==Design::ResolutionState::Changed);context.evaluatedInputsFingerprint="inputs";
  context.configurationFingerprint="changed";REQUIRE(Design::resolve(*token,context).state==Design::ResolutionState::Changed);context.configurationFingerprint="config";
  auto malformed=anchorToken(key());auto badData=malformed->data.toObject().clone();badData.set("extra",Value(true));malformed->data=Value(std::move(badData));REQUIRE(Design::resolve(*malformed,context).state==Design::ResolutionState::Invalid);
  auto axis=anchorToken(key("axis"));REQUIRE(Design::resolve(*axis,context).state==Design::ResolutionState::Unsupported);
  removeReferences("result");REQUIRE(references->resolve(*token,"result","inputs","config").state==Design::ResolutionState::Changed);REQUIRE(Design::resolve(*token,context).state==Design::ResolutionState::Changed);
}
TEST_CASE("Definition anchor references use their owned prepared defaults rather than a live instance", "[anchor-resolution]") {
  SemanticTest::Fixture fixture;fixture.evaluate(source());REQUIRE(fixture.node);auto prepared=std::make_shared<Semantic::PreparedTarget>(Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Definition,"plate"},"default-result",fixture.root.string()));REQUIRE(prepared->state==Design::ResolutionState::Resolved);
  References references("default-result",fixture.scene,"inputs","config",[prepared](Semantic::TargetKey key){REQUIRE(key.id=="plate");return *prepared;});
  auto token=anchorToken({instanceOwner("/plate","plate"),"mount"});REQUIRE(token);auto resolved=references.resolve(*token,"default-result","inputs","config");REQUIRE(resolved.state==Design::ResolutionState::Resolved);REQUIRE(resolved.frame->translation().isApprox(Vector3d(20,0,10)));REQUIRE(resolved.retainedScene->defaultDefinitionId=="plate");
  References pending("default-result",fixture.scene,"inputs","config");REQUIRE(pending.resolve(*token,"default-result","inputs","config").state==Design::ResolutionState::Unsupported);
  REQUIRE_FALSE(anchorToken({instanceOwner("/plate/child","plate"),"mount"}));
}
TEST_CASE("Retained anchor resolvers recheck captured external inputs before supplying a frame", "[anchor-resolution]") {
  SemanticTest::Fixture fixture;fixture.evaluate(source());REQUIRE(fixture.node);bool current=true;
  References references("result",fixture.scene,"inputs","config",{},[&]{return current;});auto token=anchorToken(key());REQUIRE(token);
  REQUIRE(references.resolve(*token,"result","inputs","config").frame);current=false;
  auto stale=references.resolve(*token,"result","inputs","config");REQUIRE(stale.state==Design::ResolutionState::Changed);REQUIRE_FALSE(stale.frame);
}
TEST_CASE("Canonical frames use explicit drawing Anchors and reject nonrigid defaults and duplicate purposes", "[anchor-resolution]") {
  SemanticTest::Fixture ordinary;ordinary.evaluate("cube(1);");REQUIRE(canonicalFrame(*ordinary.scene,{OwnerRole::Document,ordinary.inputs->root().fileKey}).state==Design::ResolutionState::Resolved);
  SemanticTest::Fixture rigid;rigid.evaluate(source());REQUIRE(rigid.node);REQUIRE(canonicalFrame(*rigid.scene,key().owner).frame->translation().isApprox(Vector3d(100,0,0)));
  auto scaled=source();scaled.insert(scaled.find("translate([100"),"scale([2,3,4]) ");SemanticTest::Fixture affine;affine.evaluate(scaled);REQUIRE(affine.node);REQUIRE(canonicalFrame(*affine.scene,key().owner).state==Design::ResolutionState::Unsupported);
  auto explicitFrame=source("Mount",",purpose:\"drawing\"");explicitFrame.insert(explicitFrame.find("translate([100"),"scale([2,3,4]) ");SemanticTest::Fixture drawing;drawing.evaluate(explicitFrame);REQUIRE(drawing.node);auto canonical=canonicalFrame(*drawing.scene,key().owner);REQUIRE(canonical.state==Design::ResolutionState::Resolved);REQUIRE(properRigid(*canonical.frame));REQUIRE(canonical.frame->translation().isApprox(Vector3d(260,0,40)));
  auto duplicate=source("Mount",",purpose:\"drawing\"")+"@{type:Anchor} another={id:\"other\",owner:\"/plate\",position:[0,0,0],normal:[0,0,1],up:[0,1,0],purpose:\"drawing\"};";SemanticTest::Fixture invalid;invalid.evaluate(duplicate);REQUIRE(invalid.node);REQUIRE(canonicalFrame(*invalid.scene,key().owner).state==Design::ResolutionState::Invalid);
}
