#include "core/design/DesignCollector.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignReferences.h"
#include "core/design/DesignSourceMap.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/Builtins.h"
#include "core/BuiltinContext.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/node.h"
#include "core/customizer/ParameterObject.h"
#include "openscad.h"
#include "utils/printutils.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
struct Fixture {
  std::filesystem::path directory, root;
  std::shared_ptr<EvaluationInputs> inputs;
  std::unique_ptr<SourceFileCache> cache;
  std::unique_ptr<SourceFile> parsed;
  std::unique_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> file;
  std::shared_ptr<AbstractNode> node;
  std::shared_ptr<const Design::Result> result;
  Fixture() {
    static std::atomic<unsigned> sequence{0};
    directory = std::filesystem::temp_directory_path() / ("scad-design-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence));
    std::filesystem::create_directories(directory); root = directory / "root.scad";
  }
  ~Fixture() {
    result.reset(); node.reset(); file.reset(); builtin.reset(); session.reset(); parsed.reset(); cache.reset();
    std::error_code error; std::filesystem::remove_all(directory, error);
  }
  void write(const std::string& name, const std::string& bytes) {
    std::ofstream stream(directory / name, std::ios::binary); stream << bytes; REQUIRE(stream.good());
  }
  void evaluate(const std::string& bytes, bool editors = true) {
    static const bool initialized = [] { Builtins::initialize(); return true; }(); (void)initialized;
    write("root.scad", bytes);
    inputs = std::make_shared<EvaluationInputs>(SourceInput{EvaluationInputs::fileKey(root), bytes, {}, editors ? std::optional<std::string>("root-editor") : std::nullopt, editors ? std::optional<uint64_t>(7) : std::nullopt});
    cache = std::make_unique<SourceFileCache>(inputs.get()); NativeEvaluationScope native(*inputs, *cache);
    SourceFile *source = nullptr; REQUIRE(parse(source, bytes, root.string(), root.string(), false)); parsed.reset(source); parsed->handleDependencies();
    session = std::make_unique<EvaluationSession>(directory.string());
    auto context = Context::create<BuiltinContext>(session.get()); builtin = *context;
    node = parsed->instantiate(builtin, &file); REQUIRE(file); REQUIRE(node);
    inputs->freeze(); result = session->designCollector->finish();
  }
};
std::string section(const std::string& name, const std::string& id, const std::string& extra = "") {
  return "@{type: Section}\n" + name + "={id:\"" + id + "\",origin:[0,0,height/2],normal:[0,0,1]" + extra + "};\n";
}
Design::ResolveContext resolveContext(const Design::Records& records) {
  return {"result", "inputs", "config", BoundingBox(Vector3d::Zero(), Vector3d(20,20,40)), records, {}};
}
}

TEST_CASE("Design declarations read native evaluated parameter values and retain source identity", "[design-objects]") {
  for (double height : {40.,60.}) {
    Fixture f; auto bytes = "height=" + std::to_string(height) + ";\n" + section("middle", "section-middle") + "cube([20,20,height]);";
    f.evaluate(bytes); REQUIRE(f.result->diagnostics.empty()); REQUIRE(f.result->records.size() == 1);
    auto record = f.result->records.at("section-middle"); auto payload = std::dynamic_pointer_cast<const Design::Section>(record->evaluated);
    REQUIRE(payload); REQUIRE(payload->origin.z() == height/2); REQUIRE(record->source);
    auto key = *record->source; REQUIRE(key.hostName == "middle"); REQUIRE(bytes.substr(key.beginByte, key.endByte-key.beginByte) == section("middle", "section-middle").substr(0,section("middle", "section-middle").size()-1));
    auto context = resolveContext(f.result->records); auto resolved = Design::sectionFrame(*record, context);
    REQUIRE(resolved.state == Design::ResolutionState::Resolved); REQUIRE(resolved.frame->translation().z() == height/2);
    REQUIRE(f.node->children.size() == 1);
    auto parameters = ParameterObjects::fromSourceFile(f.parsed.get(), f.file.get());
    REQUIRE(parameters.empty());
  }
}
TEST_CASE("Design includes use the root lexical parameters and use libraries export helpers only", "[design-objects]") {
  Fixture f; f.write("include.scad", section("included", "included"));
  f.write("used.scad", "height=90;" + section("hidden", "hidden") + "function create()={id:\"helper\",origin:[0,0,8],normal:[0,1,0]};");
  f.evaluate("height=60; include <include.scad> use <used.scad> @{type: Section} helper=create(); cube(height);");
  REQUIRE(f.result->records.size() == 2); REQUIRE(f.result->records.count("hidden") == 0);
  REQUIRE(std::dynamic_pointer_cast<const Design::Section>(f.result->records.at("included")->evaluated)->origin.z() == 30);
  REQUIRE(std::dynamic_pointer_cast<const Design::Section>(f.result->records.at("helper")->evaluated)->origin.z() == 8);
  REQUIRE(f.result->records.at("included")->source->fileKey == EvaluationInputs::fileKey(f.directory/"include.scad"));
}
TEST_CASE("Design type is metadata and cannot be shadowed by variables", "[design-objects]") {
  Fixture f; f.evaluate("height=40; Section=123;" + section("s", "s") + "cube(1);");
  REQUIRE(f.result->diagnostics.empty()); REQUIRE(f.result->records.at("s")->kind == "Section");
}
TEST_CASE("Design metadata scope reassignment and unknown members diagnose without changing geometry", "[design-objects]") {
  for (const auto& declaration : {
      "@{type: Unknown} s={id:\"x\"};", "@{type: \"Section\"} s={id:\"x\"};", "@{type: Section,min:0} s={id:\"x\"};",
      "@{type: Section} s={id:\"x\",origin:[0,0,0],normal:[0,0,0]};",
      "@{type: Section} s={id:\"x\",origin:[0,0,0],normal:[0,0,1],up:[0,0,2]};",
      "@{type: Section} s={id:\"x\",origin:[0,0,0],normal:[0,0,1],unknown:1};",
      "@{type: Section} s={id:\"x\",origin:[0,0,1/0],normal:[0,0,1]};",
      "@{type: Section} s={id:\"x\",origin:[0,0,0],normal:[0,0,1],adjust:{rotation:[0,0,0,0]}};",
      "@{type: View} s={id:\"x\",camera:{rotation:[0,0,0],translation:[0,0,0],distance:1,fov:40},direction:[0,1,0]};",
      "module unused(){@{type: Section} s={id:\"x\",origin:[0,0,0],normal:[0,0,1]};}",
      "@{type: Section} s={id:\"x\",origin:[0,0,0],normal:[0,0,1]}; s={id:\"y\"};",
      "module m(){@{type: Section} s={id:\"x\",origin:[0,0,0],normal:[0,0,1]};cube(1);} m();"}) {
    Fixture f; f.evaluate(std::string(declaration) + "cube(2);");
    REQUIRE_FALSE(f.result->diagnostics.empty()); REQUIRE(f.result->records.empty()); REQUIRE_FALSE(f.node->children.empty());
  }
}
TEST_CASE("Design duplicate IDs invalidate both kinds with no winner and root registries stay isolated", "[design-objects][design-references]") {
  Fixture f; f.evaluate("height=40;" + section("a","same") + "@{type: View} b={id:\"same\",direction:[0,1,0],up:[0,0,1]}; cube(1);");
  REQUIRE(f.result->records.empty()); REQUIRE(f.result->invalidIds.count("same") == 1); REQUIRE(f.result->diagnostics.size() == 2);
  Fixture other; other.evaluate("height=40;" + section("different_name","same") + "cube(1);");
  REQUIRE(other.result->records.count("same") == 1); REQUIRE(other.result->records.at("same")->label == "different_name");
}
TEST_CASE("Section frame applies normalized quaternion then translated rotated normal offset", "[design-objects]") {
  Fixture f; f.evaluate("height=40;" + section("s","s", ",up:[0,1,0],adjust:{translation:[1,2,3],rotation:[0,2,0,2],offset:5}") + "cube(1);");
  REQUIRE(f.result->diagnostics.empty()); auto context = resolveContext(f.result->records);
  auto frame = Design::sectionFrame(*f.result->records.at("s"), context);
  REQUIRE(frame.frame->translation().isApprox(Vector3d(6,2,23))); REQUIRE(frame.frame->linear().col(2).isApprox(Vector3d::UnitX()));
  REQUIRE(frame.frame->linear().determinant() == Catch::Approx(1));
}
TEST_CASE("View fixed camera roundtrips existing conventions and fit rejects unavailable bounds", "[design-objects]") {
  Fixture f; f.evaluate("@{type: View} fixed={id:\"fixed\",projection:\"perspective\",camera:{rotation:[31,42,53],translation:[1,2,3],distance:80,fov:40}};"
    "@{type: View} fitted={id:\"fit\",direction:[0,1,0],up:[0,0,1],margin:0.1}; cube(1);");
  REQUIRE(f.result->diagnostics.empty()); auto context = resolveContext(f.result->records);
  auto fixed = Design::viewCamera(*f.result->records.at("fixed"),context); REQUIRE(fixed);
  REQUIRE(fixed->getVpr().isApprox(Vector3d(31,42,53))); REQUIRE(fixed->getVpt().isApprox(Vector3d(1,2,3))); REQUIRE(fixed->zoomValue() == 80); REQUIRE(fixed->fovValue() == 40);
  auto fit = Design::viewCamera(*f.result->records.at("fit"),context); REQUIRE(fit); REQUIRE(fit->getVpt().isApprox(context.documentBounds.center()));
  REQUIRE(fit->projection == Camera::ProjectionType::ORTHOGONAL);
  context.documentBounds.setEmpty(); REQUIRE_FALSE(Design::viewCamera(*f.result->records.at("fit"),context));
}
TEST_CASE("Scoped consumer handlers receive actual values for each native invocation without global ID insertion", "[design-objects]") {
  struct State { int calls=0; std::vector<double> heights; std::vector<std::shared_ptr<const Context>> scopes; };
  auto state = std::make_shared<State>(); std::weak_ptr<State> weak = state;
  Design::KindHandler handler{"NativeTestConsumer", {Design::DeclarationHost::Assignment,Design::DeclarationHost::ModuleCall,Design::DeclarationHost::ModuleDefinition}, {Design::DeclarationScope::File,Design::DeclarationScope::Local}, Design::decodeSection, Design::RecordDestination::Consumer,
    [weak](Design::Record record,const Design::EvaluatedDeclaration& declaration,EvaluationSession&) {
      if (auto state=weak.lock()) { ++state->calls; state->heights.push_back(std::dynamic_pointer_cast<const Design::Section>(record.evaluated)->origin.z()); state->scopes.push_back(declaration.nativeContext->get_shared_ptr()); }
    }};
  REQUIRE(Design::registerKind(std::move(handler)));
  REQUIRE_FALSE(Design::registerKind({"BadConsumer",{Design::DeclarationHost::Assignment},{Design::DeclarationScope::File},Design::decodeSection,Design::RecordDestination::Consumer,{}}));
  Fixture f; f.write("used.scad", "@{type: NativeTestConsumer} top={id:\"same\",origin:[0,0,99],normal:[0,0,1]};"
    "module test(height){@{type: NativeTestConsumer} local={id:\"same\",origin:[0,0,height],normal:[0,0,1]};cube(1);}");
  f.evaluate("use <used.scad> test(7); test(13);");
  REQUIRE(state->calls == 2); REQUIRE(state->heights == std::vector<double>{7,13}); REQUIRE(state->scopes[0] != state->scopes[1]); REQUIRE(f.result->records.empty());
  state.reset();
}
TEST_CASE("Design reference outcomes protect cycles fingerprints unsupported domains and rigid frames", "[design-references]") {
  Fixture f; f.evaluate("height=40;" + section("a","a",",frame:{domain:\"design\",id:\"b\"}") + section("b","b",",frame:{domain:\"design\",id:\"a\"}") + "cube(1);");
  auto context = resolveContext(f.result->records); Design::ReferenceToken reference{"design","a",Value::undefined.clone()};
  REQUIRE(Design::resolve(reference,context).state == Design::ResolutionState::Invalid); REQUIRE(context.visitedIds.empty());
  reference.id="missing"; REQUIRE(Design::resolve(reference,context).state == Design::ResolutionState::Missing);
  reference.domain="semantic"; REQUIRE(Design::resolve(reference,context).state == Design::ResolutionState::Unsupported);
  ObjectType token(nullptr); token.set("inputsFingerprint",Value("old")); token.set("configurationFingerprint",Value("config"));
  Design::ReferenceToken geometric{"geometric","",Value(std::move(token))}; REQUIRE(Design::resolve(geometric,context).state == Design::ResolutionState::Changed);
}
TEST_CASE("Persistence closure deduplicates dependencies preserves included ownership and fails atomically", "[design-references]") {
  Fixture f; f.evaluate("height=40;"+section("base","base")+section("a","a",",frame:{domain:\"design\",id:\"base\"}")+section("b","b",",frame:{domain:\"design\",id:\"base\"}")+"cube(1);");
  Design::Records records;
  for (auto& [id,record] : f.result->records) {
    auto unsourced=std::make_shared<Design::Record>(); unsourced->id=id;unsourced->kind=record->kind;unsourced->label=record->label;unsourced->definition=Design::ownedValue(record->definition);unsourced->evaluated=record->evaluated;
    for (const auto& token:record->dependencies) unsourced->dependencies.push_back({token.domain,token.id,Design::ownedValue(token.data)});
    records.emplace(id,unsourced);
  }
  auto closure = Design::persistenceClosure({"a","b"},records,{}); REQUIRE(closure.valid()); REQUIRE(closure.insert.size()==3); REQUIRE(closure.insert.front()->id=="base");
  REQUIRE_FALSE(Design::persistenceClosure({"missing"},records,{}).valid());
  REQUIRE_FALSE(Design::persistenceClosure({"a"},records,{}, {"section_base"}).valid());
  REQUIRE_FALSE(Design::persistenceClosure({"a"},f.result->records,{}).valid());
  auto sourced=Design::persistenceClosure({"a","b"},f.result->records,{EvaluationInputs::fileKey(f.root)}); REQUIRE(sourced.valid());REQUIRE(sourced.insert.empty()); REQUIRE(sourced.sourced.size()==3);
}
TEST_CASE("Synthetic declaration hosts validate independently and consumer lifetime remains bounded", "[design-objects]") {
  int calls=0; auto live=std::make_shared<int>(1);std::weak_ptr<int> weak=live;
  REQUIRE(Design::registerKind({"SyntheticHost",{Design::DeclarationHost::ModuleDefinition,Design::DeclarationHost::ModuleCall},{Design::DeclarationScope::File},Design::decodeSection,Design::RecordDestination::Consumer,
    [weak,&calls](Design::Record,const Design::EvaluatedDeclaration&,EvaluationSession&){if(weak.lock())++calls;}}));
  REQUIRE_FALSE(Design::registerKind({"SyntheticHost",{Design::DeclarationHost::Assignment},{Design::DeclarationScope::File},Design::decodeSection}));
  REQUIRE_FALSE(Design::registerKind({"Section",{Design::DeclarationHost::Assignment},{Design::DeclarationScope::File},Design::decodeSection}));
  Fixture f;f.evaluate("height=40;"+section("s","s")+"cube(1);");auto record=f.result->records.at("s");
  Design::Collector collector(f.inputs.get());auto key=*record->source;key.host=Design::DeclarationHost::ModuleDefinition;key.hostName="module-definition";
  collector.dispatch("SyntheticHost",record->definition,{key,Design::DeclarationScope::File,f.file.get(),f.parsed->scope.get(),Design::SourceOrigin::Root},*f.session);
  key.host=Design::DeclarationHost::ModuleCall;collector.dispatch("SyntheticHost",record->definition,{key,Design::DeclarationScope::File,f.file.get(),f.parsed->scope.get(),Design::SourceOrigin::Root},*f.session);
  REQUIRE(calls==2);REQUIRE(collector.finish()->records.empty());
  collector.dispatch("Section",record->definition,{key,Design::DeclarationScope::File,f.file.get(),f.parsed->scope.get(),Design::SourceOrigin::Root},*f.session);REQUIRE(collector.finish()->diagnostics.size()==1);
  live.reset();collector.dispatch("SyntheticHost",record->definition,{key,Design::DeclarationScope::File,f.file.get(),f.parsed->scope.get(),Design::SourceOrigin::Root},*f.session);REQUIRE(calls==2);
}
TEST_CASE("Design values are read once and native GUI and headless captures agree", "[design-objects]") {
  std::string source="function once()=echo(\"DESIGN_ONCE\") {id:\"once\",origin:[0,0,4],normal:[0,0,1]}; @{type: Section} s=once();cube(1);";
  for(bool editor:{false,true}){
    int count=0;auto oldHandler=outputhandler;auto oldData=outputhandler_data;
    struct Restore { OutputHandlerFunc *a;void *data;~Restore(){set_output_handler(a,nullptr,data);} } restore{oldHandler,oldData};
    set_output_handler([](const Message& message,void *data){if(message.msg.find("DESIGN_ONCE")!=std::string::npos)++*static_cast<int *>(data);},nullptr,&count);
    Fixture f;f.evaluate(source,editor);REQUIRE(count==1);REQUIRE(f.result->records.at("once")->definition.toString().find("4")!=std::string::npos);
  }
}
TEST_CASE("Geometric references retain exact fingerprints and reject nonrigid Section frames", "[design-references]") {
  REQUIRE(Design::registerResolver("test-nonrigid",[](const Design::ReferenceToken&,Design::ResolveContext&){Transform3d frame=Transform3d::Identity();frame.linear()(0,0)=2;return Design::Resolution{Design::ResolutionState::Resolved,frame,{},""};}));
  REQUIRE_FALSE(Design::registerResolver("test-nonrigid",[](const Design::ReferenceToken&,Design::ResolveContext&){return Design::Resolution{};}));
  Fixture f;f.evaluate("height=40;"+section("s","s",",frame:{domain:\"test-nonrigid\"}")+"cube(1);");auto context=resolveContext(f.result->records);
  REQUIRE(Design::sectionFrame(*f.result->records.at("s"),context).state==Design::ResolutionState::Invalid);
  ObjectType data(nullptr);data.set("inputsFingerprint",Value("inputs"));data.set("configurationFingerprint",Value("config"));
  VectorType point(nullptr),normal(nullptr);for(double value:{1.,2.,3.})point.emplace_back(Value(value));for(double value:{0.,0.,1.})normal.emplace_back(Value(value));
  data.set("point",Value(std::move(point)));data.set("normal",Value(std::move(normal)));Design::ReferenceToken token{"geometric","",Value(std::move(data))};
  auto resolution=Design::resolve(token,context);REQUIRE(resolution.state==Design::ResolutionState::Resolved);REQUIRE(resolution.frame->translation().isApprox(Vector3d(1,2,3)));
  context.configurationFingerprint="changed";REQUIRE(Design::resolve(token,context).state==Design::ResolutionState::Changed);
}
TEST_CASE("Identity survives rename moving includes and serialization precision while cycles fail closure", "[design-references]") {
  Fixture original;original.evaluate("height=40;"+section("old_name","id",",label:\"Old\"")+"cube(1);");
  Fixture moved;moved.write("moved.scad",section("new_name","id",",label:\"New\""));moved.evaluate("height=40;include <moved.scad> cube(1);");
  REQUIRE(original.result->records.at("id")->id==moved.result->records.at("id")->id);REQUIRE(moved.result->records.at("id")->label=="New");
  Fixture cyclic;cyclic.evaluate("height=40;"+section("s","s",",frame:{domain:\"design\",id:\"s\"}")+"cube(1);");REQUIRE_FALSE(Design::persistenceClosure({"s"},cyclic.result->records,{EvaluationInputs::fileKey(cyclic.root)}).valid());
  Fixture precise;precise.evaluate("height=40;"+section("s","precision",",adjust:{offset:0.12345678901234566}")+"cube(1);");
  auto serialized=Design::serialize(*precise.result->records.at("precision"));Fixture reopened;reopened.evaluate("height=40;"+serialized+"cube(1);");
  auto before=std::dynamic_pointer_cast<const Design::Section>(precise.result->records.at("precision")->evaluated);auto after=std::dynamic_pointer_cast<const Design::Section>(reopened.result->records.at("precision")->evaluated);REQUIRE(before->offset==after->offset);
}
TEST_CASE("Registered consumers share one required transient declaration in a topological closure", "[design-references]") {
  REQUIRE(Design::registerKind({"ClosureConsumer",{Design::DeclarationHost::Assignment},{Design::DeclarationScope::Local},Design::decodeSection,Design::RecordDestination::Consumer,
    [](Design::Record,const Design::EvaluatedDeclaration&,EvaluationSession&) {}}));
  Fixture f;f.evaluate("height=40;"+section("s","shared")+"cube(1);");
  auto source=f.result->records.at("shared");auto shared=std::make_shared<Design::Record>();shared->id=source->id;shared->kind=source->kind;shared->definition=Design::ownedValue(source->definition);shared->evaluated=source->evaluated;
  Design::Records records{{shared->id,shared}};
  for(const std::string id:{"owner-one","owner-two"}){
    auto consumer=std::make_shared<Design::Record>();consumer->id=id;consumer->kind="ClosureConsumer";consumer->dependencies.push_back({"design","shared",Value::undefined.clone()});records.emplace(id,consumer);
  }
  auto closure=Design::persistenceClosure({"owner-one","owner-two"},records,{});
  REQUIRE(closure.valid());REQUIRE(closure.insert.size()==3);REQUIRE(closure.insert.front()->id=="shared");
  REQUIRE(std::count_if(closure.insert.begin(),closure.insert.end(),[](const auto& record){return record->id=="shared";})==1);
}
