#include "core/semantic/SemanticTestFixture.h"
#include "core/TransformNode.h"
using SemanticTest::Fixture;
using SemanticTest::part;
using SemanticTest::pair;
TEST_CASE("Semantic affine placement preserves Part local geometry without inverse world matrices", "[semantic-partition]") {
  for (const auto& transform : {"rotate([20,30,40])", "mirror([1,0,0])", "scale([2,3,4])", "scale([0,2,3])"}) {
    Fixture fixture; fixture.evaluate(part + "translate([10,20,30]) " + transform + " @{type:Component,id:\"a\"} bracket(width=45);");
    REQUIRE(fixture.scene->valid()); const auto& instance = fixture.scene->instances.at("/a");
    REQUIRE(instance.localToWorld.matrix().allFinite()); REQUIRE(instance.localToParent.matrix().isApprox(instance.localToWorld.matrix()));
    REQUIRE(fixture.bounds(instance.localRoot).max().isApprox(Vector3d(45,10,3)));
  }
  Fixture nested; nested.evaluate(SemanticTest::pairDefinition + "scale([0,2,3]) @{type:Component,id:\"pair\"} pair();");
  REQUIRE(nested.scene->valid()); REQUIRE(nested.scene->instances.at("/pair/right").localToParent.translation().isApprox(Vector3d(60,0,0)));
}
TEST_CASE("Semantic boundaries reject Components inside Parts cross boundary operations and anonymous Assembly output", "[semantic-partition]") {
  Fixture child; child.evaluate(part + "@{type:Part,id:\"outer\"} module outer(){@{type:Component,id:\"nested\"} bracket();} @{type:Component,id:\"o\"} outer();");
  REQUIRE_FALSE(child.node); REQUIRE(child.diagnostic("component-inside-part"));
  for (const auto& operation : {"difference", "union", "intersection", "hull", "minkowski", "render", "projection"}) {
    Fixture fixture; fixture.evaluate(part + operation + "(){@{type:Component,id:\"a\"} bracket(); cube(1);}");
    INFO(operation); REQUIRE_FALSE(fixture.node); REQUIRE(fixture.diagnostic("unsupported-component-composition"));
  }
  Fixture anonymous; anonymous.evaluate("@{type:Assembly,id:\"assembly\"} module assembly(){cube(1);} @{type:Component,id:\"a\"} assembly();");
  REQUIRE_FALSE(anonymous.node); REQUIRE(anonymous.diagnostic("assembly-anonymous-geometry"));
  Fixture root; root.evaluate("@{type:Part,id:\"p\"} module p(){!cube(1);} @{type:Component,id:\"a\"} p();");
  REQUIRE_FALSE(root.node); REQUIRE(root.diagnostic("unsupported-part-root"));
  Fixture ordinary; ordinary.evaluate("difference(){cube(2);cube(1);} module p(){cube(1);} union(){p();p();}"); REQUIRE(ordinary.node); REQUIRE(ordinary.scene->valid()); REQUIRE(ordinary.scene->instances.empty());
}
TEST_CASE("Semantic residual partition keeps ordinary geometry and native background highlight roles", "[semantic-partition]") {
  Fixture fixture; fixture.evaluate(part + "% @{type:Component,id:\"background\"} bracket(); # translate([40,0,0]) @{type:Component,id:\"highlight\"} bracket(); translate([100,0,0]) cube(2);");
  REQUIRE(fixture.scene->valid()); REQUIRE(fixture.scene->parts.size() == 2); REQUIRE(fixture.scene->residual.size() == 1);
  REQUIRE(fixture.scene->instances.at("/background").background); REQUIRE(fixture.scene->instances.at("/highlight").highlight);
  REQUIRE(fixture.bounds(fixture.scene->residual[0].root).min().isApprox(Vector3d(100,0,0)));
}
TEST_CASE("Semantic placement passes run once deterministically preserve local roots and update native provenance", "[semantic-evaluation][semantic-partition]") {
  std::vector<std::string> order;
  auto first = Semantic::registerPlacementPass("test.first", 1, [&](Semantic::PlacementContext&) { order.push_back("first"); return Semantic::PlacementOutcome{}; }); REQUIRE(first);
  auto second = Semantic::registerPlacementPass("test.second", 2, [&](Semantic::PlacementContext& context) {
    order.push_back("second"); REQUIRE(context.instances().size() == 3);
    REQUIRE(context.instances().at("/pair/right").originalLocalToParent.translation().isApprox(Vector3d(60,0,0)));
    Transform3d correction = Transform3d::Identity(); correction.translation() = Vector3d(0,20,0);
    return context.insertTransform("/pair/right", correction);
  }); REQUIRE(second);
  REQUIRE_FALSE(Semantic::registerPlacementPass("test.second", 0, [](auto&){return Semantic::PlacementOutcome{};}));
  Fixture fixture; fixture.evaluate(pair); REQUIRE(fixture.scene->valid()); REQUIRE(fixture.node);
  REQUIRE((order == std::vector<std::string>{"first", "second"}));
  const auto& right = fixture.scene->instances.at("/pair/right"); REQUIRE(right.localToWorld.translation().isApprox(Vector3d(60,20,0)));
  REQUIRE(fixture.bounds(right.localRoot).max().isApprox(Vector3d(45,10,3))); REQUIRE(fixture.bounds(fixture.node).max().isApprox(Vector3d(105,30,3)));
  REQUIRE(fixture.scene->syntheticCalls.size() == 1); REQUIRE_FALSE(fixture.scene->syntheticCalls[0]->isRoot()); REQUIRE_FALSE(fixture.scene->syntheticCalls[0]->isBackground());
  size_t linked=0; for (auto index : fixture.provenance->instance(right.invocationId)->emittedNodeIndices) {
    auto mapping = fixture.provenance->forNode(index); if (mapping.localToWorld && mapping.localToWorld->translation().isApprox(Vector3d(60,20,0))) ++linked;
  }
  REQUIRE(linked >= 2);
  auto frozen = fixture.session->semanticCollector->finish(); REQUIRE(order.size() == 2); REQUIRE(frozen->instances.at("/pair/right").localToWorld.matrix().isApprox(right.localToWorld.matrix()));
}
TEST_CASE("Semantic placement failures discard the new tree and every scope unwinds", "[semantic-evaluation][semantic-partition]") {
  int calls=0;
  auto registration = Semantic::registerPlacementPass("test.failure", 1, [&](Semantic::PlacementContext& context) {
    ++calls; Transform3d correction=Transform3d::Identity(); correction.translation()=Vector3d(100,0,0);
    auto inserted=context.insertTransform("/pair/right",correction); REQUIRE(inserted.success);
    return Semantic::PlacementOutcome::failure("fake-placement-failure", "test failure after mutation");
  }); REQUIRE(registration);
  Fixture fixture; fixture.evaluate(pair); REQUIRE_FALSE(fixture.node); REQUIRE_FALSE(fixture.scene->valid()); REQUIRE(fixture.diagnostic("fake-placement-failure")); REQUIRE(calls == 1);
  registration = {};
  Fixture exception; exception.evaluate("@{type:Part,id:\"p\"} module p(){assert(false,\"stop\");cube(1);} @{type:Component,id:\"a\"} p();");
  REQUIRE_FALSE(exception.node); REQUIRE(exception.session->semanticCollector->activeDepth() == 0);
  Fixture fresh; fresh.evaluate(pair); REQUIRE(fresh.scene->valid()); REQUIRE(fresh.scene->syntheticCalls.empty());
  REQUIRE(fresh.scene->instances.at("/pair/right").localToWorld.translation().isApprox(Vector3d(60,0,0)));
}
TEST_CASE("Semantic corrected root envelopes cannot be discarded by native root extraction", "[semantic-partition]") {
  auto registration=Semantic::registerPlacementPass("test.root",1,[](Semantic::PlacementContext& context){Transform3d correction=Transform3d::Identity();correction.translation()=Vector3d(1,0,0);return context.insertTransform("/a",correction);}); REQUIRE(registration);
  Fixture fixture; fixture.evaluate(part + "! @{type:Component,id:\"a\"} bracket();"); REQUIRE_FALSE(fixture.node); REQUIRE(fixture.diagnostic("unsupported-placement-root"));
}
