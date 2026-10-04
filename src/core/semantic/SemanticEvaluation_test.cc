#include "core/semantic/SemanticTestFixture.h"
#include "core/semantic/SemanticEvaluation.h"
using SemanticTest::Fixture;
TEST_CASE("Semantic definition defaults instantiate the retained declaration in a separate native session", "[semantic-targets][semantic-evaluation]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair);
  auto original = fixture.node; auto scene = fixture.scene;
  auto job = Semantic::evaluateDefaults(*scene, "bracket"); INFO(job.diagnostic); REQUIRE(job.valid());
  REQUIRE(job.session.get() != fixture.session.get()); REQUIRE(job.root != original);
  REQUIRE(fixture.bounds(job.root).max().isApprox(Vector3d(30,10,3)));
  REQUIRE(job.scene->instances.at("/bracket").parameters.at("width").value->toDouble() == 30);
  REQUIRE(job.scene->instances.at("/bracket").invocationId != 0);
  REQUIRE(fixture.scene->instances.at("/pair/right").parameters.at("width").value->toDouble() == 45);
  auto assembly = Semantic::evaluateDefaults(*scene, "pair-def"); INFO(assembly.diagnostic); REQUIRE(assembly.valid());
  REQUIRE(assembly.scene->parts.size() == 2); REQUIRE(fixture.bounds(assembly.root).max().isApprox(Vector3d(105,10,3)));
}
TEST_CASE("Semantic retained default jobs use captured library globals even after source deletion", "[semantic-targets]") {
  Fixture fixture; fixture.write("library.scad", "size=12; @{type:Part,id:\"library-part\"} module p(width=size){cube([width,2,3]);}");
  fixture.evaluate("use <library.scad> @{type:Component,id:\"a\"} p(width=40);"); REQUIRE(fixture.scene->valid());
  fixture.write("library.scad", "size=999; module p(){cube(999);}");
  auto job = Semantic::evaluateDefaults(*fixture.scene, "library-part"); INFO(job.diagnostic); REQUIRE(job.valid()); REQUIRE(fixture.bounds(job.root).max().isApprox(Vector3d(12,2,3)));
  std::filesystem::remove(fixture.directory / "library.scad");
  auto deleted = Semantic::evaluateDefaults(*fixture.scene, "library-part"); INFO(deleted.diagnostic); REQUIRE(deleted.valid()); REQUIRE(fixture.bounds(deleted.root).max().isApprox(Vector3d(12,2,3)));
  auto absent = Semantic::evaluateDefaults(*fixture.scene, "absent"); REQUIRE_FALSE(absent.valid()); REQUIRE_FALSE(absent.diagnostic.empty());
}
TEST_CASE("Semantic default jobs dispatch placement once preserve local geometry and copy preview configuration", "[semantic-targets][semantic-evaluation]") {
  Fixture fixture; fixture.evaluate("@{type:Part,id:\"p\"} module p(width=$preview?5:8){cube(width);} @{type:Component,id:\"a\"} p();");
  int passes=0;
  auto registration = Semantic::registerPlacementPass("default.test", 1, [&](Semantic::PlacementContext& context) {
    ++passes; REQUIRE(context.instances().size() == 1); Transform3d correction = Transform3d::Identity(); correction.translation() = Vector3d(20,0,0);
    return context.insertTransform("/p", correction);
  }); REQUIRE(registration);
  auto job = Semantic::evaluateDefaults(*fixture.scene, "p", [](Context& context){context.set_variable("$preview", Value(true));}); INFO(job.diagnostic); REQUIRE(job.valid()); REQUIRE(passes == 1);
  REQUIRE(fixture.bounds(job.scene->instances.at("/p").localRoot).max().isApprox(Vector3d(5,5,5)));
  REQUIRE(fixture.bounds(job.root).min().isApprox(Vector3d(20,0,0)));
  REQUIRE(job.scene->instances.at("/p").localToWorld.translation().isApprox(Vector3d(20,0,0)));
  REQUIRE(job.scene->syntheticCalls.size() == 2);
  auto immutable = job.session->semanticCollector->finish(); REQUIRE(passes == 1); REQUIRE(immutable->valid());
}
