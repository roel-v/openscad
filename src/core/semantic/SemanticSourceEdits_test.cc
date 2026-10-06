#include "core/semantic/SemanticSourceEdits.h"
#include "core/semantic/SemanticTestFixture.h"
#include <algorithm>
using namespace Semantic;
namespace {
std::string apply(std::string bytes,ParameterEdit edit) {
  REQUIRE(edit.editable);REQUIRE(edit.reason.empty());REQUIRE_FALSE(edit.patches.empty());
  std::sort(edit.patches.begin(),edit.patches.end(),[](const auto& a,const auto& b){return a.span.begin>b.span.begin;});
  for(const auto& patch:edit.patches){REQUIRE(bytes.substr(patch.span.begin,patch.span.end-patch.span.begin)==patch.expected);bytes.replace(patch.span.begin,patch.span.end-patch.span.begin,patch.replacement);}
  return bytes;
}
const std::string definition="@{type:Part,id:\"p\"} module p(width=30,height=5){cube([width,height,2]);}\n";
const std::string prefix="@{type:Component,id:\"a\"} ";
}
TEST_CASE("Structure initialization preserves globals imports geometry bytes and names","[semantic-source-edits]") {
  SemanticTest::Fixture f;
  f.write("library.scad","module imported() { cube([size,2,3]); }\n");
  const std::string text="\xef\xbb\xbf// header\r\ninclude <library.scad>\r\nsize=9; // Customizer\r\nmodule scadmaxx_model(){}\r\n// geometry\r\nimported(); // keep Unicode α\r\ntranslate([20,0,0]) cube(2);";
  f.evaluate(text);const auto before=f.bounds(f.node);
  auto changed=apply(text,initializeStructure(*f.scene,f.inputs->root().fileKey,"Housing \"α\""));
  REQUIRE(changed.find("module scadmaxx_model_1()")!=std::string::npos);
  REQUIRE(changed.substr(0,text.find("imported();"))==text.substr(0,text.find("imported();")));
  REQUIRE(changed.find(text.substr(text.find("imported();")))!=std::string::npos);
  f.evaluate(changed);REQUIRE(f.scene->valid());REQUIRE(f.scene->instances.size()==1);
  REQUIRE(f.scene->instances.begin()->second.label=="Housing \"α\"");
  REQUIRE(f.bounds(f.node).min().isApprox(before.min()));REQUIRE(f.bounds(f.node).max().isApprox(before.max()));
  REQUIRE_FALSE(initializeStructure(*f.scene,f.inputs->root().fileKey,"Again").editable);
}
TEST_CASE("Structure initialization refuses source that cannot preserve scope or presentation","[semantic-source-edits]") {
  SemanticTest::Fixture f;
  for(const auto& text:{"cube(1); a=3;","{cube(1);}","cube(1); module later(){}","echo($parent_modules);cube(1);","cube(is_undef($children)?1:2);","#cube(1);","%cube(1);","!cube(1);","x=1;"}){
    f.evaluate(text);auto edit=initializeStructure(*f.scene,f.inputs->root().fileKey,"Part");REQUIRE_FALSE(edit.editable);REQUIRE(edit.patches.empty());REQUIRE_FALSE(edit.reason.empty());
  }
  f.write("geometry.scad","cube(2);");f.evaluate("include <geometry.scad>\ncube(1);");
  REQUIRE_FALSE(initializeStructure(*f.scene,f.inputs->root().fileKey,"Part").editable);
}
TEST_CASE("Semantic source edits insert omitted overrides and replace only native winning literal spans","[semantic-source-edits]") {
  for(const auto& [call,expected]:std::vector<std::pair<std::string,std::string>>{
      {"p();","p(width = 40);"},{"p(30);","p(40);"},{"p(width = 30);","p(width = 40);"},
      {"p(height=5,);","p(height=5, width = 40,);"}}){
    SemanticTest::Fixture f;const auto text=definition+prefix+call;f.evaluate(text);REQUIRE(f.scene->valid());
    const auto& parameter=f.scene->instances.at("/a").parameters.at("width");
    REQUIRE(apply(text,changeParameter(*f.scene,{TargetRole::Instance,"/a"},parameter,Value(40.0)))==definition+prefix+expected);
  }
}
TEST_CASE("Semantic source insertion preserves BOM CRLF comments trailing commas and Unicode","[semantic-source-edits]") {
  SemanticTest::Fixture f;
  const std::string text="\xef\xbb\xbf"+definition+prefix+"p(\r\n    height = 5 // Keep α\r\n); // End\r\n";
  f.evaluate(text);REQUIRE(f.scene->valid());const auto& parameter=f.scene->instances.at("/a").parameters.at("width");
  REQUIRE(apply(text,changeParameter(*f.scene,{TargetRole::Instance,"/a"},parameter,Value(40.0)))=="\xef\xbb\xbf"+definition+prefix+"p(\r\n    height = 5, // Keep α\r\n    width = 40\r\n); // End\r\n");
}
TEST_CASE("Semantic source reset removes a named override and its comma without deleting comments","[semantic-source-edits]") {
  for(const auto& [call,expected]:std::vector<std::pair<std::string,std::string>>{
      {"p(width=40);","p();"},{"p(width=40, /* keep */ height=5);","p( /* keep */ height=5);"},
      {"p(height=5, /* keep */ width=40);","p(height=5 /* keep */ );"},
      {"p(height=5,width=40,);","p(height=5,);"}}){
    SemanticTest::Fixture f;const auto text=definition+prefix+call;f.evaluate(text);const auto& parameter=f.scene->instances.at("/a").parameters.at("width");
    REQUIRE(apply(text,resetParameter(*f.scene,{TargetRole::Instance,"/a"},parameter))==definition+prefix+expected);
  }
}
TEST_CASE("Semantic source expressions ambiguous bindings and positional reset stay read only","[semantic-source-edits]") {
  for(const auto& call:{"p(width=30+10);","p(width=-/*keep*/30);","p(width=30,width=40);"}){
    SemanticTest::Fixture f;f.evaluate(definition+prefix+call);const auto& parameter=f.scene->instances.at("/a").parameters.at("width");
    const auto edit=changeParameter(*f.scene,{TargetRole::Instance,"/a"},parameter,Value(50.0));REQUIRE_FALSE(edit.editable);REQUIRE(edit.patches.empty());REQUIRE_FALSE(edit.reason.empty());
  }
  SemanticTest::Fixture f;f.evaluate(definition+prefix+"p(30);");REQUIRE_FALSE(resetParameter(*f.scene,{TargetRole::Instance,"/a"},f.scene->instances.at("/a").parameters.at("width")).editable);
}
TEST_CASE("Semantic source shared call guards count hidden and root excluded native invocations","[semantic-source-edits]") {
  SemanticTest::Fixture f;
  f.evaluate(definition+"@{type:Assembly,id:\"rack\"} module rack(width=40){@{type:Component,id:\"piece\"} p(width=30);}\n@{type:Component,id:\"rack-a\"} rack(width=40); !@{type:Component,id:\"rack-b\"} rack();");
  REQUIRE(f.scene->valid());REQUIRE(f.scene->instances.size()==4);
  const auto& child=f.scene->instances.at("/rack-a/piece").parameters.at("width");
  const auto refused=changeParameter(*f.scene,{TargetRole::Instance,"/rack-a/piece"},child,Value(50.0));REQUIRE_FALSE(refused.editable);REQUIRE(refused.reason.find("multiple instances")!=std::string::npos);
  REQUIRE(changeParameter(*f.scene,{TargetRole::Instance,"/rack-a"},f.scene->instances.at("/rack-a").parameters.at("width"),Value(50.0)).editable);
}
TEST_CASE("Semantic definition defaults deliberately patch only the native parameter expression","[semantic-source-edits]") {
  SemanticTest::Fixture f;const auto text=definition+prefix+"p();";f.evaluate(text);
  const auto defaults=evaluateDefaults(*f.scene,"p");REQUIRE(defaults.valid());
  const auto& parameter=defaults.scene->instances.at("/p").parameters.at("width");
  REQUIRE(apply(text,changeParameter(*f.scene,{TargetRole::Definition,"p"},parameter,Value(45.0)))=="@{type:Part,id:\"p\"} module p(width=45,height=5){cube([width,height,2]);}\n"+prefix+"p();");
  f.evaluate("base=30;@{type:Part,id:\"p\"} module p(width=base){cube(width);}"+prefix+"p();");
  REQUIRE_FALSE(inspectParameter(*f.scene,{TargetRole::Definition,"p"},f.scene->instances.at("/a").parameters.at("width")).editable);
  // A computed default does not prevent a unique call from adding a literal override.
  REQUIRE(changeParameter(*f.scene,{TargetRole::Instance,"/a"},f.scene->instances.at("/a").parameters.at("width"),Value(40.0)).editable);
}
TEST_CASE("Semantic literal encoding roundtrips booleans finite numbers Unicode strings and flat vectors","[semantic-source-edits]") {
  REQUIRE(encodeLiteral(Value(true))=="true");REQUIRE(encodeLiteral(Value(-0.125))=="-0.125");
  REQUIRE(encodeLiteral(Value(std::string("α\"\\\n\t")))=="\"α\\\"\\\\\\n\\t\"");
  VectorType vector(nullptr);vector.emplace_back(1.0);vector.emplace_back(-2.5);REQUIRE(encodeLiteral(Value(std::move(vector)))=="[1, -2.5]");
  REQUIRE_FALSE(encodeLiteral(Value(std::numeric_limits<double>::infinity())));REQUIRE_FALSE(encodeLiteral(Value::undefined));
  VectorType nested(nullptr);nested.emplace_back("computed");REQUIRE_FALSE(encodeLiteral(Value(std::move(nested))));
}
