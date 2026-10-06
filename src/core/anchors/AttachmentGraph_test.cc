#include "core/anchors/AttachmentTestFixture.h"
#include <catch2/catch_test_macros.hpp>
using namespace Anchors;
using namespace AnchorTest;
TEST_CASE("Attachment graph combines containment and cross-branch targets in stable path order", "[attachment-graph]") {
  auto native=instances({{"/a",""},{"/a/child","/a"},{"/b",""},{"/c",""}});ReferenceScene scene;
  add(scene,relation("second","/c","/a/child"));add(scene,relation("first","/a","/b"));
  auto graph=attachmentGraph(native,scene.attachments);REQUIRE(graph.valid());REQUIRE(graph.order==std::vector<std::string>{"","/b","/a","/a/child","/c"});
  add(scene,relation("internal","/a/child","/a"));graph=attachmentGraph(native,scene.attachments);REQUIRE(graph.valid());REQUIRE(graph.order==std::vector<std::string>{"","/b","/a","/a/child","/c"});
}
TEST_CASE("Attachment graph rejects self descendant ancestor sibling cycles and multiple drivers with sources", "[attachment-graph]") {
  auto native=instances({{"/a",""},{"/a/child","/a"},{"/b",""},{"/b/child","/b"}});ReferenceScene scene;
  SECTION("self") {add(scene,relation("self","/a","/a"));}
  SECTION("descendant drives ancestor") {add(scene,relation("ancestor","/a","/a/child"));}
  SECTION("sibling cycle") {add(scene,relation("one","/a","/b"));add(scene,relation("two","/b","/a"));}
  SECTION("cross containment cycle") {add(scene,relation("one","/a","/b/child"));add(scene,relation("two","/b","/a/child"));}
  SECTION("two drivers") {add(scene,relation("one","/a","/b"));add(scene,relation("two","/a","/b/child"));}
  auto graph=attachmentGraph(native,scene.attachments);REQUIRE_FALSE(graph.valid());REQUIRE_FALSE(graph.diagnostics.empty());
  for(const auto& diagnostic:graph.diagnostics){REQUIRE(diagnostic.source);REQUIRE(diagnostic.source->beginByte==10);REQUIRE(diagnostic.message.find("/a")!=std::string::npos);}
}
TEST_CASE("Ordinary empty attachment graphs are valid and unknown endpoints cannot create placement", "[attachment-graph]") {
  ReferenceScene scene;auto graph=attachmentGraph({},scene.attachments);REQUIRE(graph.valid());REQUIRE(graph.order==std::vector<std::string>{""});
  add(scene,relation("missing","/missing",""));graph=attachmentGraph({},scene.attachments);REQUIRE_FALSE(graph.valid());
}
