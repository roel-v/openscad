#include "core/semantic/SemanticTestFixture.h"
#include "core/semantic/SemanticEvaluation.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignCollector.h"
#include "utils/printutils.h"
#include <set>
using SemanticTest::Fixture;
using SemanticTest::part;
using SemanticTest::pairDefinition;
using SemanticTest::pair;

namespace {struct ExtensionProbe : Design::Payload {std::vector<std::string> owners,defaultContexts;};}
TEST_CASE("Consumer evaluation state publishes owned extensions for ordinary and semantic results", "[semantic-evaluation]") {
  Design::KindHandler handler;handler.name="SemanticExtensionProbe";handler.hosts={Design::DeclarationHost::Assignment};handler.scopes={Design::DeclarationScope::File,Design::DeclarationScope::Local};
  handler.decode=[](const Value&,const Design::DeclarationContext&){Design::DecodeResult result;result.payload=std::make_shared<Design::Payload>();return result;};handler.destination=Design::RecordDestination::Consumer;
  handler.consume=[](Design::Record,const Design::EvaluatedDeclaration& declaration,EvaluationSession& session){
    auto& state=session.consumerStates["extension-probe"];if(!state)state=std::make_shared<ExtensionProbe>();
    auto owner=Semantic::currentScope(session,declaration.nativeContext,declaration.localScope);REQUIRE(owner);std::static_pointer_cast<ExtensionProbe>(state)->owners.push_back(owner->instancePath);std::static_pointer_cast<ExtensionProbe>(state)->defaultContexts.push_back(owner->defaultDefinitionId);
  };REQUIRE(Design::registerKind(std::move(handler)));
  auto pass=Semantic::registerPlacementPass("extension-probe",900,[](Semantic::PlacementContext& context){
    auto state=std::dynamic_pointer_cast<ExtensionProbe>(context.consumerState("extension-probe"));if(!state)return Semantic::PlacementOutcome{};
    return context.publishExtension("extension-probe",std::make_shared<const ExtensionProbe>(*state));
  });REQUIRE(pass);
  std::shared_ptr<const ExtensionProbe> retained;
  {Fixture fixture;fixture.evaluate("@{type:SemanticExtensionProbe} a={id:\"a\"}; cube(3);");REQUIRE(fixture.node);REQUIRE_FALSE(fixture.scene->marked);retained=std::dynamic_pointer_cast<const ExtensionProbe>(fixture.scene->extensions.at("extension-probe"));REQUIRE(retained->owners==std::vector<std::string>{""});}
  REQUIRE(retained->owners.size()==1);
  Fixture placed;placed.evaluate("@{type:Part,id:\"p\"} module p(){@{type:SemanticExtensionProbe} a={id:\"a\"};cube(1);} @{type:Component,id:\"a\"} p(); @{type:Component,id:\"b\"} p();");REQUIRE(placed.node);
  auto payload=std::dynamic_pointer_cast<const ExtensionProbe>(placed.scene->extensions.at("extension-probe"));REQUIRE(payload->owners==std::vector<std::string>{"/a","/b"});REQUIRE(placed.scene->defaultDefinitionId.empty());
  auto defaults=Semantic::evaluateDefaults(*placed.scene,"p");REQUIRE(defaults.valid());REQUIRE(defaults.scene->defaultDefinitionId=="p");auto defaultPayload=std::dynamic_pointer_cast<const ExtensionProbe>(defaults.scene->extensions.at("extension-probe"));REQUIRE(defaultPayload->owners==std::vector<std::string>{"/p"});REQUIRE(defaultPayload->defaultContexts==std::vector<std::string>{"p"});
}
TEST_CASE("A failed ordinary extension pass aborts before exposing usable geometry", "[semantic-evaluation]") {
  auto pass=Semantic::registerPlacementPass("ordinary-extension-failure",901,[](Semantic::PlacementContext& context){
    auto first=context.publishExtension("duplicate-probe",std::make_shared<const Design::Payload>());REQUIRE(first.success);
    return context.publishExtension("duplicate-probe",std::make_shared<const Design::Payload>());
  });REQUIRE(pass);Fixture fixture;fixture.evaluate("cube(1);");REQUIRE_FALSE(fixture.node);REQUIRE(fixture.diagnostic("placement-extension-id"));REQUIRE(fixture.scene->extensions.empty());
}

TEST_CASE("Semantic native instances own full paths exact parameter bindings and finished Part roots", "[semantic-evaluation][semantic-partition]") {
  Fixture fixture; fixture.evaluate(pair); REQUIRE(fixture.scene->valid()); REQUIRE(fixture.node);
  REQUIRE(fixture.scene->instances.size() == 3); REQUIRE(fixture.scene->parts.size() == 2); REQUIRE(fixture.scene->residual.empty());
  const auto& right = fixture.scene->instances.at("/pair/right");
  REQUIRE(right.parentPath == "/pair"); REQUIRE(right.definitionId == "bracket"); REQUIRE(right.displayed);
  REQUIRE(right.parameters.at("width").value->toDouble() == 45); REQUIRE(right.parameters.at("width").source->originalExpression == "width+10");
  REQUIRE(right.localToParent.translation().isApprox(Vector3d(60,0,0))); REQUIRE(right.localToWorld.translation().isApprox(Vector3d(60,0,0)));
  REQUIRE(fixture.bounds(right.localRoot).max().isApprox(Vector3d(45,10,3))); REQUIRE(fixture.bounds(fixture.node).max().isApprox(Vector3d(105,10,3)));
  REQUIRE(right.invocationId != 0); REQUIRE(fixture.provenance->instance(right.invocationId)->call.syntax == right.call->syntax);
  for (const auto& [index, owner] : fixture.scene->partOwners) REQUIRE((owner == "/pair/left" || owner == "/pair/right"));
}
TEST_CASE("Semantic nested rack identity survives key reordering labels deletion and escaped IDs", "[semantic-evaluation]") {
  const std::string rack = pairDefinition + "@{type:Assembly,id:\"rack-def\"} module rack(width=35){for(side=[\"front\",\"back\"]) translate([0,side==\"front\"?0:25,0]) @{type:Component,id:side} pair(width=width);}\n";
  Fixture nested; nested.evaluate(rack + "@{type:Component,id:\"rack-a\"} rack(); translate([200,0,0]) @{type:Component,id:\"rack-b\"} rack(width=45);");
  REQUIRE(nested.scene->valid()); REQUIRE(nested.scene->instances.size() == 14); REQUIRE(nested.scene->parts.size() == 8);
  REQUIRE(nested.scene->instances.at("/rack-b/back/right").parameters.at("width").value->toDouble() == 55);
  std::set<std::string> firstPaths;
  for (const auto& keys : {"[\"a/b\",\"c~d\"]", "[\"c~d\",\"a/b\"]"}) {
    Fixture loop; loop.evaluate(part + "for(key=" + keys + ") @{type:Component,id:key,label:str(\"label \",key)} bracket();");
    REQUIRE(loop.scene->valid()); std::set<std::string> paths; for (const auto& [path, value] : loop.scene->instances) paths.insert(path);
    if (firstPaths.empty()) firstPaths = paths; else REQUIRE(paths == firstPaths);
    REQUIRE((paths == std::set<std::string>{"/a~1b", "/c~0d"}));
  }
  Fixture deleted; deleted.evaluate(part + "for(key=[\"a/b\"]) @{type:Component,id:key,label:\"Renamed\"} bracket();");
  REQUIRE(deleted.scene->instances.size() == 1); REQUIRE(deleted.scene->instances.at("/a~1b").label == "Renamed");
  Fixture duplicates; duplicates.evaluate(part + "for(key=[\"a\",\"a\"]) @{type:Component,id:key} bracket();"); REQUIRE_FALSE(duplicates.node); REQUIRE(duplicates.diagnostic("duplicate-component-id"));
  REQUIRE((Semantic::pathSegments("/a~1b/c~0d") == std::optional<std::vector<std::string>>{{"a/b", "c~d"}}));
  REQUIRE_FALSE(Semantic::pathSegments("/bad~2")); REQUIRE_FALSE(Semantic::pathSegments("/empty/"));
}
TEST_CASE("Semantic actual lexical modules and use defaults determine Component ownership", "[semantic-evaluation]") {
  Fixture fixture;
  fixture.write("left.scad", "default_width=12; @{type:Part,id:\"left-definition\"} module p(width=default_width){cube(width);} module make_left(){@{type:Component,id:\"left\"} p();}");
  fixture.write("right.scad", "default_width=23; @{type:Part,id:\"right-definition\"} module p(width=default_width){cube(width);} module make_right(){@{type:Component,id:\"right\"} p();}");
  fixture.evaluate("use <left.scad> use <right.scad> make_left(); make_right();"); REQUIRE(fixture.scene->valid());
  REQUIRE(fixture.scene->instances.at("/left").definitionId == "left-definition"); REQUIRE(fixture.scene->instances.at("/right").definitionId == "right-definition");
  REQUIRE(fixture.scene->instances.at("/left").parameters.at("width").value->toDouble() == 12);
  REQUIRE(fixture.scene->instances.at("/right").parameters.at("width").value->toDouble() == 23);
}
TEST_CASE("Semantic disabled branches and root exclusion preserve full edit multiplicity", "[semantic-evaluation][semantic-partition]") {
  Fixture fixture; fixture.evaluate(part + "if(false) @{type:Component,id:\"absent\"} bracket(); * @{type:Component,id:\"disabled\"} bracket(); for(id=[\"a\",\"b\"]) @{type:Component,id:id} bracket(); ! @{type:Component,id:\"root\"} bracket();");
  REQUIRE(fixture.scene->valid()); REQUIRE(fixture.scene->instances.size() == 3); REQUIRE(fixture.scene->parts.size() == 1);
  REQUIRE_FALSE(fixture.scene->instances.at("/a").displayed); REQUIRE(fixture.scene->instances.at("/root").displayed);
  const auto& a = fixture.scene->instances.at("/a"); REQUIRE(fixture.scene->callInstances.at(a.call->syntax).size() == 2);
  REQUIRE(fixture.provenance->instances().size() > fixture.scene->instances.size());
}
TEST_CASE("Semantic body scope exists before evaluated declarations and rejects helper ownership", "[semantic-evaluation]") {
  auto captured = std::make_shared<std::vector<Semantic::ScopeOwner>>();
  Design::KindHandler handler; handler.name = "SemanticTestScope";
  handler.hosts = {Design::DeclarationHost::Assignment}; handler.scopes = {Design::DeclarationScope::File,Design::DeclarationScope::Local};
  handler.decode = [](const Value&, const Design::DeclarationContext&) { Design::DecodeResult result; result.payload = std::make_shared<Design::Payload>(); return result; };
  handler.destination = Design::RecordDestination::Consumer;
  handler.consume = [captured](Design::Record, const Design::EvaluatedDeclaration& declaration, EvaluationSession& session) {
    auto owner = Semantic::currentScope(session, declaration.nativeContext, declaration.localScope); REQUIRE(owner); captured->push_back(*owner);
  };
  REQUIRE(Design::registerKind(std::move(handler)));
  Fixture fixture; fixture.evaluate("@{type:SemanticTestScope} f={id:\"file\"}; module helper(){@{type:SemanticTestScope} h={id:\"helper\"};} @{type:Part,id:\"p\"} module p(){@{type:SemanticTestScope} a={id:\"direct\"}; helper(); cube(1);} @{type:Component,id:\"a\"} p(); @{type:Component,id:\"b\"} p();");
  const auto& owners = *captured;
  REQUIRE(fixture.scene->valid()); REQUIRE(owners.size() == 5); REQUIRE(owners[0].instancePath.empty());
  REQUIRE(owners[1].instancePath == "/a"); REQUIRE(owners[1].isDirectModuleBody); REQUIRE_FALSE(owners[2].isDirectModuleBody);
  REQUIRE(owners[3].instancePath == "/b"); REQUIRE(owners[3].invocationId != owners[1].invocationId);
  REQUIRE_FALSE(Semantic::currentScope(*fixture.session, fixture.file.get(), nullptr));
}
