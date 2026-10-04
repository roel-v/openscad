#include "core/semantic/SemanticCollector.h"
#include "core/semantic/SemanticKinds.h"
#include "core/design/DesignCollector.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignSourceFields.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/EvaluationInputs.h"
#include "core/EvaluationSession.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/ModuleInstantiation.h"
#include "core/UserModule.h"
#include "core/ScopeContext.h"
#include "core/Tree.h"
#include "core/node.h"
#include "geometry/GeometryEvaluator.h"
#include "openscad.h"
#include "utils/printutils.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
struct Fixture {
  std::filesystem::path directory, root;
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceProvenance::SyntaxTable> syntax;
  std::unique_ptr<SourceFileCache> cache;
  std::unique_ptr<SourceFile> parsed;
  std::unique_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> file;
  std::shared_ptr<AbstractNode> node;
  std::shared_ptr<const Semantic::Scene> scene;
  Fixture() {
    static std::atomic<unsigned> sequence{0};
    directory = std::filesystem::absolute(std::filesystem::temp_directory_path() /
      ("scad-semantic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence)));
    REQUIRE(std::filesystem::create_directories(directory)); root = directory / "root.scad";
  }
  ~Fixture() {
    scene.reset(); node.reset(); file.reset(); builtin.reset(); session.reset(); parsed.reset(); cache.reset(); syntax.reset();
    std::error_code error; std::filesystem::remove_all(directory, error);
  }
  void write(const std::string& name, const std::string& text) {
    std::ofstream stream(directory / name, std::ios::binary); stream << text; REQUIRE(stream.good());
  }
  bool parseText(const std::string& text) {
    inputs = std::make_shared<EvaluationInputs>(SourceInput{EvaluationInputs::fileKey(root), text, {}, "semantic-editor", 1});
    syntax = std::make_shared<SourceProvenance::SyntaxTable>(inputs);
    cache = std::make_unique<SourceFileCache>(inputs.get());
    NativeEvaluationScope native(*inputs, *cache); SourceProvenance::ParseScope spans(syntax.get());
    SourceFile *file = nullptr; auto success = parse(file, text, root.string(), root.string(), false); parsed.reset(file);
    if (success) parsed->handleDependencies(); return success;
  }
  void evaluate(const std::string& text) {
    static const bool initialized=[] { Builtins::initialize(); return true; }(); (void)initialized;
    REQUIRE(parseText(text));
    NativeEvaluationScope native(*inputs, *cache); SourceProvenance::ParseScope spans(syntax.get());
    session = std::make_unique<EvaluationSession>(directory.string());
    auto context = Context::create<BuiltinContext>(session.get()); builtin = *context;
    AbstractNode::resetIndexCounter(); node = parsed->instantiate(builtin, &file);
    scene = session->semanticCollector->finish(); inputs->freeze();
  }
  bool diagnostic(const std::string& code) const {
    for (const auto& value : scene->diagnostics) if (value.code == code) return true;
    return false;
  }
};
const std::string part="@{type: Part,id:\"bracket\",label:\"Bracket\"} module bracket(width=30) {cube([width,10,3]);}\n";
const std::string pair=part+"@{type: Assembly,id:\"pair\"} module pair(width=35,gap=60) {"
  "@{type: Component,id:\"left\"} bracket(width=width); translate([gap,0,0]) {"
  "@{type: Component,id:\"right\"} bracket(width=width+10);}} @{type: Component,id:\"pair\"} pair();";
}

TEST_CASE("Semantic definitions are typed inert templates and explicit calls retain native geometry", "[semantic-grammar][semantic-declarations]") {
  Fixture inert; inert.evaluate(part); REQUIRE(inert.scene->valid()); REQUIRE(inert.node); REQUIRE(inert.node->children.empty());
  REQUIRE(inert.scene->definitions.size()==1); REQUIRE(inert.scene->definitions.at("bracket").label=="Bracket");
  Fixture placed; placed.evaluate(pair); REQUIRE(placed.scene->valid()); REQUIRE(placed.scene->definitions.size()==2);
  Tree tree(placed.node); GeometryEvaluator evaluator(tree); auto geometry=evaluator.evaluateGeometry(*placed.node,true); REQUIRE(geometry);
  auto bounds=geometry->getBoundingBox(); REQUIRE(bounds.min().isApprox(Vector3d(0,0,0))); REQUIRE(bounds.max().isApprox(Vector3d(105,10,3)));
  auto design=placed.session->designCollector->finish(); REQUIRE(design->records.size()==2); REQUIRE(design->records.at("pair")->kind=="Assembly");
}
TEST_CASE("Semantic metadata uses bare registered names and literal definition identities", "[semantic-grammar]") {
  for (const auto& text : {
    "@{type:\"Part\",id:\"x\"} module p() {}",
    "@{type:Part} module p() {}",
    "@{type:Part,id:\"\"} module p() {}",
    "id=\"x\"; @{type:Part,id:id} module p() {}",
    "@{type:Part,id:\"x\",label:1} module p() {}",
    "@{type:Part,id:\"x\",bad:1} module p() {}",
    "@{type:Part,id:\"x\",id:\"y\"} module p() {}",
    "@{type:Section,id:\"x\"} module p() {}",
    "@{type:Missing,id:\"x\"} module p() {}",
    "@{type:Part,id:\"x\"} cube(1);"}) {
    Fixture invalid; invalid.evaluate(text); INFO(text); REQUIRE_FALSE(invalid.node); REQUIRE_FALSE(invalid.scene->valid()); REQUIRE(invalid.diagnostic("invalid-semantic-metadata"));
  }
  Fixture variables; variables.evaluate("Part=\"Other\"; Component=123;"+part+"@{type:Component,id:\"a\"} bracket();");
  REQUIRE(variables.scene->valid()); REQUIRE(variables.node);
}
TEST_CASE("Semantic native calls require marked targets metadata and no supplied children", "[semantic-declarations]") {
  for (const auto& entry : {std::make_pair(part+"bracket();", "component-required"),
      std::make_pair(std::string("@{type:Component,id:\"x\"} cube(1);"), "component-target"),
      std::make_pair(std::string("@{type:Component,id:\"x\"} missing();"), "component-target"),
      std::make_pair(part+"@{type:Component,id:\"x\"} bracket() {}", "component-children"),
      std::make_pair(part+"@{type:Component,id:\"x\"} bracket() cube(1);", "component-children")}) {
    Fixture invalid; invalid.evaluate(entry.first); INFO(entry.first); REQUIRE_FALSE(invalid.node); REQUIRE(invalid.diagnostic(entry.second));
  }
  Fixture numeric; numeric.evaluate(part+"@{type:Component,id:2} bracket();"); REQUIRE_FALSE(numeric.node); REQUIRE(numeric.diagnostic("id-required"));
}
TEST_CASE("Semantic discovery rejects local definitions redeclarations and cross-kind identity collisions", "[semantic-declarations]") {
  Fixture local; local.evaluate("module helper(){"+part+"} cube(1);"); REQUIRE(local.diagnostic("local-semantic-definition")); REQUIRE_FALSE(local.node);
  Fixture redefined; redefined.evaluate(part+"module bracket() {} cube(1);"); REQUIRE(redefined.diagnostic("semantic-redefinition")); REQUIRE_FALSE(redefined.node);
  Fixture duplicate; duplicate.evaluate(part+"@{type:Part,id:\"bracket\"} module second(){} @{type:Part,id:\"bracket\"} module third(){}");
  REQUIRE(duplicate.scene->invalidIds.count("bracket")); REQUIRE(duplicate.scene->definitions.empty()); REQUIRE_FALSE(duplicate.node);
  Fixture section; section.evaluate(part+"@{type:Section} s={id:\"bracket\",origin:[0,0,0],normal:[0,0,1]};");
  REQUIRE(section.scene->invalidIds.count("bracket")); REQUIRE_FALSE(section.node); REQUIRE(section.diagnostic("duplicate-id"));
}
TEST_CASE("Semantic use discovery imports templates without executing unused library output", "[semantic-declarations]") {
  int echoes=0; auto oldHandler=outputhandler; auto oldData=outputhandler_data;
  struct Restore {OutputHandlerFunc *handler;void *data;~Restore(){set_output_handler(handler,nullptr,data);}} restore{oldHandler,oldData};
  set_output_handler([](const Message& value,void *data){if(value.msg.find("UNUSED_LIBRARY")!=std::string::npos)++*static_cast<int *>(data);},nullptr,&echoes);
  Fixture fixture; fixture.write("library.scad",part+"echo(\"UNUSED_LIBRARY\"); cube(999);");
  fixture.evaluate("use <library.scad> use <library.scad> cube(1);"); REQUIRE(fixture.scene->valid()); REQUIRE(fixture.scene->definitions.size()==1); REQUIRE(echoes==0);
  REQUIRE(fixture.session->designCollector->finish()->records.at("bracket")->source->host==Design::DeclarationHost::ModuleDefinition);
  Tree tree(fixture.node); GeometryEvaluator evaluator(tree); REQUIRE(evaluator.evaluateGeometry(*fixture.node,true)->getBoundingBox().max().isApprox(Vector3d(1,1,1)));
}
TEST_CASE("Semantic annotation spans preserve exact BOM CRLF UTF8 parameters and call modifiers", "[semantic-grammar]") {
  Fixture fixture; const std::string definition="@{type:Part,id:\"\xc3\xa9\"}\r\nmodule p(width /*keep*/ = 30) {cube(width);}";
  const std::string call="! @{type:Component,id:\"a\"} p(width = 40);";
  const std::string text="\xef\xbb\xbf// header\r\n"+definition+"\r\n"+call;
  fixture.evaluate(text); REQUIRE(fixture.scene->valid()); auto& d=fixture.scene->definitions.at("\xc3\xa9"); REQUIRE(d.syntax);
  REQUIRE(fixture.syntax->original(d.syntax->whole)==definition); REQUIRE(d.syntax->arguments.size()==1);
  REQUIRE(d.syntax->arguments[0].originalExpression=="30");
  auto nativeCall=fixture.parsed->scope->moduleInstantiations.at(0); REQUIRE(nativeCall->isRoot());
  auto site=fixture.syntax->site(*nativeCall); REQUIRE(site); REQUIRE(fixture.syntax->original(site->whole)==call);
  REQUIRE(site->arguments[0].originalExpression=="40"); std::ostringstream printed; nativeCall->print(printed, "");
  REQUIRE(printed.str().find("@{ type: Component")!=std::string::npos);
}
TEST_CASE("Ordinary models and Section View assignment hosts remain independent of semantic discovery", "[semantic-grammar][semantic-declarations]") {
  Fixture ordinary; ordinary.evaluate("module p(size=2) {cube(size);} p(3);"); REQUIRE(ordinary.node); REQUIRE(ordinary.scene->valid()); REQUIRE_FALSE(ordinary.scene->marked); REQUIRE(ordinary.scene->definitions.empty());
  Fixture mixed; mixed.evaluate(part+"@{type:Section} s={id:\"s\",origin:[0,0,0],normal:[0,0,1]}; @{type:Component,id:\"p\"} bracket();");
  REQUIRE(mixed.scene->valid()); REQUIRE(mixed.session->designCollector->finish()->records.size()==2);
  auto handler=Design::kind("Section"); REQUIRE(handler->hosts==std::set<Design::DeclarationHost>{Design::DeclarationHost::Assignment});
}
TEST_CASE("Semantic module hosts cannot be mutated by assignment field editing", "[semantic-declarations]") {
  Fixture fixture; fixture.evaluate(part);
  auto record=fixture.session->designCollector->finish()->records.at("bracket");
  auto map=Design::DesignSourceMap::scan(*record->source, fixture.inputs->root(), "Part");
  REQUIRE_FALSE(map.direct()); REQUIRE_FALSE(map.ownsDeclaration());
  std::string error; REQUIRE_FALSE(map.deletion(error));
  REQUIRE_FALSE(map.patches({{"label","\"Changed\"",Design::LiteralKind::String}},error));
}
