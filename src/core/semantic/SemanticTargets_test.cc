#include "core/semantic/SemanticTestFixture.h"
#include "core/semantic/SemanticTargets.h"
#include "core/design/DesignCollector.h"
#include "core/design/DesignKinds.h"
using SemanticTest::Fixture;
TEST_CASE("Semantic targets distinguish local placed Assembly and definition default geometry", "[semantic-targets]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair);
  auto right=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/pair/right"},"result",fixture.directory.string()); REQUIRE((right.state == Design::ResolutionState::Resolved));
  REQUIRE(right.localBounds->min().isApprox(Vector3d(0,0,0))); REQUIRE(right.localBounds->max().isApprox(Vector3d(45,10,3)));
  REQUIRE(right.worldBounds->min().isApprox(Vector3d(60,0,0))); REQUIRE(right.worldBounds->max().isApprox(Vector3d(105,10,3)));
  auto assembly=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/pair"},"result",fixture.directory.string()); REQUIRE((assembly.state == Design::ResolutionState::Resolved)); REQUIRE(assembly.localBounds->max().isApprox(Vector3d(105,10,3)));
  auto definition=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Definition,"bracket"},"result",fixture.directory.string()); INFO(definition.diagnostic); REQUIRE((definition.state == Design::ResolutionState::Resolved));
  REQUIRE(definition.localBounds->max().isApprox(Vector3d(30,10,3))); REQUIRE(definition.localToWorld.matrix().isIdentity()); REQUIRE(definition.parameters.at("width").value->toDouble() == 30);
}
TEST_CASE("Semantic target resolvers prepare once and bind only an exact successful result", "[semantic-targets][design-references]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair); int requests=0; std::optional<Semantic::TargetKey> requested;
  auto targets=std::make_shared<Semantic::Targets>("result",fixture.scene,[&](auto key){++requests;requested=key;});
  auto token=Semantic::targetToken({Semantic::TargetRole::Instance,"/pair/right"});
  REQUIRE((targets->resolve(token,"result").state == Design::ResolutionState::Unsupported)); REQUIRE(requests == 1);
  targets->resolve(token,"result"); REQUIRE(requests == 1); REQUIRE(requested);
  auto ready=std::make_shared<Semantic::PreparedTarget>(Semantic::prepareTarget(fixture.scene,*requested,"result",fixture.directory.string())); REQUIRE(targets->install(ready));
  REQUIRE((targets->resolve(token,"changed").state == Design::ResolutionState::Changed)); REQUIRE((targets->resolve(token,"result").state == Design::ResolutionState::Resolved));
  Semantic::publishTargets(targets); Design::Records records; Design::ResolveContext context{"result","inputs","config",BoundingBox(),records,{}};
  auto resolved=Design::resolve(token,context); REQUIRE((resolved.state == Design::ResolutionState::Resolved)); REQUIRE(resolved.frame->translation().isApprox(Vector3d(60,0,0))); REQUIRE(resolved.bounds->max().isApprox(Vector3d(105,10,3)));
  Semantic::removeTargets("result"); REQUIRE((Design::resolve(token,context).state == Design::ResolutionState::Changed));
  auto missing=Semantic::targetToken({Semantic::TargetRole::Instance,"/pair/missing"}); REQUIRE((targets->resolve(missing,"result").state == Design::ResolutionState::Missing)); REQUIRE(requests == 1);
}
TEST_CASE("Semantic references reject malformed roles fields paths and collisions without retargeting", "[semantic-targets]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair); Semantic::Targets targets("result",fixture.scene,{});
  auto reference=[](ObjectType data) {
    ObjectType value(nullptr); value.set("domain",Value("semantic")); value.set("id",Value("/pair/right")); value.set("data",Value(std::move(data)));
    return *Design::referenceToken(Value(std::move(value)),"");
  };
  for (const auto& role : {"bad", "", "Component"}) { ObjectType data(nullptr); data.set("role",Value(role)); auto token=reference(std::move(data)); REQUIRE((targets.resolve(token,"result").state == Design::ResolutionState::Invalid)); }
  ObjectType data(nullptr); data.set("role",Value("instance")); data.set("extra",Value(1)); auto extra=reference(std::move(data)); REQUIRE((targets.resolve(extra,"result").state == Design::ResolutionState::Invalid));
  for (const auto& path : {"pair/right", "/a~2b", "/pair/", ""}) REQUIRE_FALSE(Semantic::targetKey(Semantic::targetToken({Semantic::TargetRole::Instance,path})));
  Fixture collision; collision.evaluate(SemanticTest::part + "@{type:Part,id:\"bracket\"} module other(){}"); Semantic::Targets invalid("result",collision.scene,{});
  REQUIRE((invalid.resolve(Semantic::targetToken({Semantic::TargetRole::Definition,"bracket"}),"result").state == Design::ResolutionState::Invalid));
}
TEST_CASE("Semantic nonrigid frames remain affine and cross sections stay in world space", "[semantic-targets][design-references]") {
  Fixture fixture; fixture.evaluate(SemanticTest::part + "scale([2,3,4]) @{type:Component,id:\"a\"} bracket(); @{type:Section} s={id:\"section\",origin:[1,2,3],normal:[0,0,1]};"); REQUIRE(fixture.scene->valid());
  auto targets=std::make_shared<Semantic::Targets>("affine",fixture.scene,std::function<void(Semantic::TargetKey)>{}); auto ready=std::make_shared<Semantic::PreparedTarget>(Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/a"},"affine",fixture.directory.string())); REQUIRE(targets->install(ready)); Semantic::publishTargets(targets);
  auto records=fixture.session->designCollector->finish()->records; Design::ResolveContext context{"affine","inputs","config",BoundingBox(),records,{}};
  auto target=Design::resolve(Semantic::targetToken({Semantic::TargetRole::Instance,"/a"}),context);
  REQUIRE((target.state == Design::ResolutionState::Resolved)); REQUIRE(target.frame);
  REQUIRE(target.frame->linear().isApprox(Vector3d(2,3,4).asDiagonal().toDenseMatrix()));
  auto frame=Design::sectionFrame(*records.at("section"),context); INFO(frame.diagnostic);
  REQUIRE((frame.state == Design::ResolutionState::Resolved)); REQUIRE(frame.frame);
  REQUIRE(frame.frame->translation().isApprox(Vector3d(1,2,3))); REQUIRE(frame.frame->linear().isIdentity());
  Semantic::removeTargets("affine");
}
TEST_CASE("Semantic cancelled target preparation cannot install an apparently ready target", "[semantic-targets]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair);
  auto cancelled=std::make_shared<Semantic::PreparedTarget>(Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/pair/right"},"result",fixture.directory.string(),{},[]{return true;}));
  REQUIRE((cancelled->state == Design::ResolutionState::Unsupported)); REQUIRE(cancelled->resultId == "result"); REQUIRE(cancelled->key.id == "/pair/right");
  Semantic::Targets targets("result",fixture.scene,{}); REQUIRE(targets.install(cancelled)); auto resolution=targets.resolve(Semantic::targetToken(cancelled->key),"result"); REQUIRE(resolution.diagnostic.find("cancelled") != std::string::npos);
}
TEST_CASE("Semantic explicit targets retain background mirrored singular and empty native shapes", "[semantic-targets]") {
  for (const auto& placement : {"%", "mirror([1,0,0])", "scale([0,1,1])", "#"}) {
    Fixture fixture; fixture.evaluate(SemanticTest::part + placement + " @{type:Component,id:\"part\"} bracket();"); REQUIRE(fixture.scene->valid());
    auto target=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/part"},"result",fixture.directory.string());
    INFO(placement); INFO(target.diagnostic); REQUIRE((target.state == Design::ResolutionState::Resolved)); REQUIRE(target.localGeometry); REQUIRE_FALSE(target.localGeometry->isEmpty());
    REQUIRE(target.placedGeometry); if (std::string(placement)!="scale([0,1,1])") REQUIRE_FALSE(target.placedGeometry->isEmpty());
  }
  Fixture empty; empty.evaluate("@{type:Part,id:\"empty\"} module empty(){} @{type:Component,id:\"empty-instance\"} empty();");
  auto target=Semantic::prepareTarget(empty.scene,{Semantic::TargetRole::Instance,"/empty-instance"},"empty",empty.directory.string()); REQUIRE((target.state == Design::ResolutionState::Resolved));
  REQUIRE((!target.localGeometry || target.localGeometry->isEmpty()));
}
