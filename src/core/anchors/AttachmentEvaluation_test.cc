#include "core/anchors/AnchorResolver.h"
#include "core/semantic/SemanticTestFixture.h"
#include "core/TransformNode.h"
#include <sstream>
using namespace Anchors;
namespace {
std::string model() {std::ifstream file(std::filesystem::path(OPENSCAD_TEST_DATA_DIR)/"scad/anchors/reusable-fixture.scad",std::ios::binary);REQUIRE(file);std::ostringstream out;out<<file.rdbuf();return out.str();}
void replace(std::string& source,const std::string& before,const std::string& after) {auto at=source.find(before);REQUIRE(at!=std::string::npos);source.replace(at,before.size(),after);}
ReferenceKey key(const std::string& path,const std::string& id="mount") {return {instanceOwner(path,""),id};}
}
TEST_CASE("Native attachment finalization places repeated Assembly pegs and preserves finished local Part geometry", "[attachment-evaluation]") {
  SemanticTest::Fixture fixture;fixture.evaluate(model());REQUIRE(fixture.node);REQUIRE(fixture.scene->valid());
  const auto& a=fixture.scene->instances.at("/fixture-a/peg");const auto& b=fixture.scene->instances.at("/fixture-b/peg");
  REQUIRE(a.localToWorld.translation().isApprox(Vector3d(20,0,10),1e-12));REQUIRE(b.localToWorld.translation().isApprox(Vector3d(130,0,10),1e-12));
  auto local=fixture.bounds(a.localRoot);REQUIRE(local.min().isApprox(Vector3d(-2,-2,0),1e-12));REQUIRE(local.max().isApprox(Vector3d(2,2,12),1e-12));
  auto all=fixture.bounds(fixture.node);REQUIRE(all.min().isApprox(Vector3d(0,-2,0),1e-12));REQUIRE(all.max().isApprox(Vector3d(160,20,22),1e-12));
  auto assembly=fixture.bounds(fixture.scene->instances.at("/fixture-b").localRoot);REQUIRE(assembly.max().isApprox(Vector3d(60,20,22),1e-12));
  auto references=referenceScene(*fixture.scene);REQUIRE(references->placements.at(a.path).matrix().isApprox(a.localToWorld.matrix(),1e-12));
  REQUIRE(fixture.scene->syntheticCalls.size()==2);size_t wrappers=0;
  std::function<void(const AbstractNode&)> visit=[&](const AbstractNode& node) {
    for(const auto& call:fixture.scene->syntheticCalls)if(node.modinst==call.get()) {
      REQUIRE(dynamic_cast<const TransformNode *>(&node));auto mapping=fixture.provenance->forNode(node.index());REQUIRE(mapping.confidence==SourceProvenance::Confidence::Multiple);REQUIRE(mapping.localToWorld);
      const auto& instance = mapping.localToWorld->matrix().isApprox(a.localToWorld.matrix(),1e-12) ? a : b;
      REQUIRE(std::find(mapping.candidateInstances.begin(),mapping.candidateInstances.end(),instance.invocationId)!=mapping.candidateInstances.end());
      const auto& emitted=fixture.provenance->instance(instance.invocationId)->emittedNodeIndices;
      REQUIRE(std::find(emitted.begin(),emitted.end(),node.index())!=emitted.end());
      REQUIRE(mapping.localToWorld->matrix().isApprox(instance.localToWorld.matrix(),1e-12));++wrappers;
    }
    for(const auto& child:node.children)visit(*child);
  };visit(*fixture.node);REQUIRE(wrappers==2);
}
TEST_CASE("Reevaluation changes one parametric target without accumulating attachment corrections", "[attachment-evaluation]") {
  auto source=model();SemanticTest::Fixture original;original.evaluate(source);REQUIRE(original.node);
  replace(source,"fixture-a\"} fixture(width = 40)","fixture-a\"} fixture(width = 50)");
  SemanticTest::Fixture changed;changed.evaluate(source);REQUIRE(changed.node);
  REQUIRE(changed.scene->instances.at("/fixture-a/peg").localToWorld.translation().isApprox(Vector3d(25,0,10)));
  REQUIRE(changed.scene->instances.at("/fixture-b/peg").localToWorld.translation().isApprox(Vector3d(130,0,10)));
  REQUIRE(original.scene->instances.at("/fixture-a/peg").localToWorld.translation().isApprox(Vector3d(20,0,10)));
  SemanticTest::Fixture again;again.evaluate(source);REQUIRE(again.node);REQUIRE(again.scene->instances.at("/fixture-a/peg").localToWorld.matrix().isApprox(changed.scene->instances.at("/fixture-a/peg").localToWorld.matrix()));
}
TEST_CASE("Native affine attachment envelopes preserve pre-attachment world distances and solved source mappings", "[attachment-evaluation]") {
  auto source=model();replace(source,"@{type: Component, id: \"fixture-a\"} fixture(width = 40);","translate([5,8,3]) rotate([12,34,56]) scale([-2,3,4]) @{type: Component, id: \"fixture-a\"} fixture(width = 40);");
  SemanticTest::Fixture fixture;fixture.evaluate(source);REQUIRE(fixture.node);
  const auto& assembly=fixture.scene->instances.at("/fixture-a");const auto& child=fixture.scene->instances.at("/fixture-a/peg");
  const Transform3d baseline=assembly.localToWorld*child.originalLocalToParent;
  for(const Vector3d difference:{Vector3d(1,0,0),Vector3d(0,1,0),Vector3d(1,2,3)})REQUIRE(std::abs((child.localToWorld.linear()*difference).norm()-(baseline.linear()*difference).norm())<1e-10);
  auto childAnchor=resolveReference(fixture.scene,key(child.path,"base"));auto targetAnchor=resolveReference(fixture.scene,key("/fixture-a/plate"));REQUIRE(childAnchor.frame);REQUIRE(targetAnchor.frame);
  REQUIRE(childAnchor.frame->translation().isApprox(targetAnchor.frame->translation(),1e-10));REQUIRE(childAnchor.frame->linear().col(2).isApprox(-targetAnchor.frame->linear().col(2),1e-10));
  REQUIRE(childAnchor.reflectedOwner);REQUIRE(properRigid(*childAnchor.frame));REQUIRE(fixture.bounds(child.localRoot).sizes().isApprox(Vector3d(4,4,12),1e-10));
}
TEST_CASE("Definition-default jobs solve relative attachments and refuse external live-instance dependencies", "[attachment-evaluation]") {
  SemanticTest::Fixture fixture;fixture.evaluate(model());REQUIRE(fixture.node);auto defaults=Semantic::evaluateDefaults(*fixture.scene,"fixture");REQUIRE(defaults.valid());
  REQUIRE(defaults.scene->instances.at("/fixture/peg").localToWorld.translation().isApprox(Vector3d(20,0,10)));
  auto bounds=fixture.bounds(defaults.root);REQUIRE(bounds.max().isApprox(Vector3d(40,20,22)));
  auto source=model();replace(source,"target: {component: \"plate\", anchor: \"mount\"}","target: {component: \"/fixture-a/plate\", anchor: \"mount\"}");
  SemanticTest::Fixture external;external.evaluate(source);REQUIRE(external.node);auto unavailable=Semantic::evaluateDefaults(*external.scene,"fixture");REQUIRE_FALSE(unavailable.valid());REQUIRE(unavailable.diagnostic.find("missing")!=std::string::npos);
}
TEST_CASE("Native cycles missing anchors and truncated root envelopes expose no usable new tree", "[attachment-evaluation]") {
  SemanticTest::Fixture fixture;auto source=model();
  SECTION("missing") {replace(source,"anchor: \"mount\"","anchor: \"deleted\"");}
  SECTION("root on driven child") {replace(source,"@{type: Component, id: \"peg\"}","!@{type: Component, id: \"peg\"}");}
  SECTION("cycle") {source+="@{type:Attachment} reverse={id:\"reverse\",child:{component:\"/fixture-a/plate\",anchor:\"mount\"},target:{component:\"/fixture-a/peg\",anchor:\"base\"}};";}
  fixture.evaluate(source);REQUIRE_FALSE(fixture.node);REQUIRE_FALSE(fixture.scene->valid());REQUIRE(fixture.scene->extensions.empty());
}
TEST_CASE("Attachment draft validation returns a candidate without modifying the retained scene", "[attachment-evaluation]") {
  SemanticTest::Fixture fixture;fixture.evaluate(model());REQUIRE(fixture.node);auto current=referenceScene(*fixture.scene);auto original=current->attachments.begin()->second;
  auto draft=original;draft.value.offset=Vector3d(0,0,5);
  auto collision=validateAttachmentDraft(draft,*current,fixture.scene->instances);REQUIRE_FALSE(collision.valid());
  auto edited=validateAttachmentDraft(draft,*current,fixture.scene->instances,original.key);REQUIRE(edited.valid());
  REQUIRE(edited.placements.at(draft.child.path).translation().isApprox(Vector3d(20,0,15)));
  REQUIRE(current->attachments.at(original.key).value.offset.isZero());REQUIRE(fixture.scene->instances.at(draft.child.path).localToWorld.translation().isApprox(Vector3d(20,0,10)));
}
