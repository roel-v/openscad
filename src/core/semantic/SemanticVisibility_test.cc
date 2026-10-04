#include "core/semantic/SemanticTestFixture.h"
#include "core/semantic/SemanticVisibility.h"
TEST_CASE("Semantic visibility respects full escaped ancestry and solo restores document eyes", "[semantic-visibility]") {
  Semantic::Visibility first,second; first.hidden.insert("/rack/left");
  REQUIRE_FALSE(first.visible("/rack/left/bracket"));REQUIRE(first.visible("/rack/leftover"));REQUIRE(second.visible("/rack/left/bracket"));
  first.hidden.insert("/rack");REQUIRE_FALSE(first.visible("/rack/right"));
  first.solo="/rack/right";REQUIRE(first.visible("/rack/right/bolt"));REQUIRE_FALSE(first.visible("/rack/left"));
  first.solo.reset();REQUIRE_FALSE(first.visible("/rack/right"));REQUIRE(first.hidden.count("/rack/left"));
  REQUIRE(Semantic::descendantOf("/a~1b/child","/a~1b"));REQUIRE_FALSE(Semantic::descendantOf("/a~1bc/child","/a~1b"));
}
TEST_CASE("Semantic valid reevaluation prunes deleted paths and preserves reordered paths", "[semantic-visibility]") {
  SemanticTest::Fixture fixture;fixture.evaluate(SemanticTest::pair);
  Semantic::Visibility state;state.hidden={"/pair/right","/gone"};state.solo="/gone";state.prune(*fixture.scene);
  REQUIRE(state.hidden==std::set<std::string>{"/pair/right"});REQUIRE_FALSE(state.solo);
  state.solo="/pair/right";state.prune(*fixture.scene);REQUIRE(state.solo=="/pair/right");
}
