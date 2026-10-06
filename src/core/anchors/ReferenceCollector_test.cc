#include "core/anchors/ReferenceCollector.h"
#include "core/semantic/SemanticTestFixture.h"
#include "core/semantic/SemanticEvaluation.h"
#include "utils/printutils.h"
#include <sstream>
using namespace Anchors;
namespace {
std::string declaration(const std::string& id="mount",const std::string& extra="") {
  return "@{type:Anchor} mount={id:\""+id+"\",position:[width/2,0,10],normal:[0,0,1],up:[0,1,0]"+extra+"};";
}
bool diagnostic(const ReferenceScene& scene,const std::string& code) {return std::any_of(scene.diagnostics.begin(),scene.diagnostics.end(),[&](const auto& d){return d.code==code;});}
ReferenceKey key(const std::string& path,const std::string& id="mount") { return {instanceOwner(path,""),id}; }
std::string model() {
  std::ifstream file(std::filesystem::path(OPENSCAD_TEST_DATA_DIR)/"scad/anchors/reusable-fixture.scad",std::ios::binary);REQUIRE(file);std::ostringstream stream;stream<<file.rdbuf();return stream.str();
}
}
TEST_CASE("Reusable Assembly references and relations collect in independent native instance namespaces", "[anchor-collection]") {
  SemanticTest::Fixture fixture;fixture.evaluate(model());REQUIRE(fixture.node);
  auto scene=referenceScene(*fixture.scene);REQUIRE(scene);REQUIRE(scene->references.size()==4);REQUIRE(scene->attachments.size()==2);
  REQUIRE(std::get<Anchor>(scene->references.at(key("/fixture-a/plate")).value).position.x()==20);
  REQUIRE(std::get<Anchor>(scene->references.at(key("/fixture-b/plate")).value).position.x()==30);
  REQUIRE(scene->drivers.count("/fixture-a/peg")==1);REQUIRE(scene->drivers.count("/fixture-b/peg")==1);
  for(const auto& [identity,count]:scene->sourceUseCounts)REQUIRE(count==2);
  for(const auto& [reference,record]:scene->references){REQUIRE(record.declaration->source);REQUIRE(record.declaration->source->endByte>record.declaration->source->beginByte);REQUIRE(record.invocationId!=0);}
  auto defaults=Semantic::evaluateDefaults(*fixture.scene,"fixture");REQUIRE(defaults.valid());auto defaultScene=referenceScene(*defaults.scene);REQUIRE(defaultScene);REQUIRE(defaultScene->defaultDefinitionId=="fixture");
  for(const auto& [reference,record]:defaultScene->references)REQUIRE(reference.owner.role==OwnerRole::DefinitionDefault);
}
TEST_CASE("File instance additions affect one owner and cannot override an inherited reference", "[anchor-collection]") {
  SemanticTest::Fixture fixture;
  const auto part="@{type:Part,id:\"p\"} module p(width=40){"+declaration()+"cube(width);}@{type:Component,id:\"a\"}p();@{type:Component,id:\"b\"}p(width=60);";
  SECTION("distinct addition") {
    fixture.evaluate(part+"@{type:Point} extra={id:\"extra\",owner:\"/a\",position:[1,2,3]};");REQUIRE(fixture.node);auto scene=referenceScene(*fixture.scene);REQUIRE(scene->references.size()==3);REQUIRE(scene->references.count(key("/a","extra")));REQUIRE_FALSE(scene->references.count(key("/b","extra")));
  }
  SECTION("duplicate") {
    fixture.evaluate(part+"@{type:Point} extra={id:\"mount\",owner:\"/a\",position:[1,2,3]};");REQUIRE(fixture.node);auto scene=referenceScene(*fixture.scene);REQUIRE(scene->invalidReferences.count(key("/a")));REQUIRE_FALSE(scene->references.count(key("/a")));REQUIRE(scene->references.count(key("/b")));REQUIRE(diagnostic(*scene,"duplicate-reference"));
  }
}
TEST_CASE("Ordinary document references retain helper values Unicode IDs and separate labels", "[anchor-collection]") {
  SemanticTest::Fixture fixture;
  fixture.evaluate("function make(w)={id:\"安装\",label:\"Mount label\",position:[w/2,0,10],normal:[0,0,1],up:[0,1,0]};@{type:Anchor} mount=make(60);@{type:Axis} axis={id:\"axis\",position:[0,0,0],direction:[1,0,0]};@{type:Plane} plane={id:\"plane\",position:[0,0,0],normal:[0,0,1]};cube(1);");
  REQUIRE(fixture.node);REQUIRE_FALSE(fixture.scene->marked);auto scene=referenceScene(*fixture.scene);REQUIRE(scene->references.size()==3);
  ReferenceKey reference{{OwnerRole::Document,scene->documentPath},"安装"};const auto& record=scene->references.at(reference);REQUIRE(record.label=="Mount label");REQUIRE(std::get<Anchor>(record.value).position.x()==30);
}
TEST_CASE("Reference ownership accepts encoded full paths and rejects traversal wildcards and missing owners", "[anchor-collection]") {
  REQUIRE(resolvePath("child~1x~0y","/assembly")=="/assembly/child~1x~0y");REQUIRE(resolvePath("","/assembly")=="/assembly");
  REQUIRE_FALSE(resolvePath("../child","/assembly"));REQUIRE_FALSE(resolvePath("*","/assembly"));REQUIRE_FALSE(resolvePath("bad~2","/assembly"));
  SemanticTest::Fixture fixture;fixture.evaluate("@{type:Part,id:\"p\"} module p(width=2){"+declaration()+"cube(width);}@{type:Component,id:\"组/一~x\"}p();@{type:Point} add={id:\"p\",owner:\"/组~1一~0x\",position:[0,0,0]};@{type:Point} missing={id:\"missing\",owner:\"/deleted\",position:[0,0,0]};");
  REQUIRE(fixture.node);auto scene=referenceScene(*fixture.scene);REQUIRE(scene->references.size()==2);REQUIRE(scene->references.count(key("/组~1一~0x","p")));REQUIRE(diagnostic(*scene,"anchor-owner"));
}
TEST_CASE("Nested helper control transform and Part explicit-owner declarations reject ambiguous scope", "[anchor-collection]") {
  const std::vector<std::string> bodies={"module helper(){"+declaration()+"}helper();","if(true){"+declaration()+"}","translate([1,0,0]){"+declaration()+"}",declaration("mount",",owner:\"/a\"")};
  for(const auto& body:bodies){SemanticTest::Fixture fixture;fixture.evaluate("@{type:Part,id:\"p\"} module p(width=2){"+body+"cube(1);}@{type:Component,id:\"a\"}p();");REQUIRE(fixture.node);auto scene=referenceScene(*fixture.scene);REQUIRE(scene);REQUIRE(scene->references.empty());REQUIRE(diagnostic(*scene,"anchor-scope"));}
}
TEST_CASE("Use libraries exclude top-level metadata and retain invoked module parameters", "[anchor-collection]") {
  SemanticTest::Fixture fixture;fixture.write("library.scad","width=999;"+declaration("unused")+"@{type:Part,id:\"p\"} module p(width=40){"+declaration()+"cube(width);}");
  fixture.evaluate("use <library.scad>;@{type:Component,id:\"a\"}p(width=60);");REQUIRE(fixture.node);auto scene=referenceScene(*fixture.scene);REQUIRE(scene->references.size()==1);REQUIRE(std::get<Anchor>(scene->references.at(key("/a")).value).position.x()==30);
}
TEST_CASE("Part Attachments malformed relations wrong reference kinds and duplicate drivers fail atomically", "[anchor-collection]") {
  SemanticTest::Fixture fixture;
  SECTION("Part scope") {fixture.evaluate("@{type:Part,id:\"p\"}module p(){@{type:Attachment} a={id:\"a\",child:{component:\"x\",anchor:\"a\"},target:{component:\"x\",anchor:\"a\"}};cube(1);}@{type:Component,id:\"p\"}p();");}
  SECTION("malformed") {fixture.evaluate("@{type:Attachment} a={id:\"a\",child:{component:\"x\",anchor:\"a\"},target:{component:\"x\",anchor:\"a\"},unknown:1};cube(1);");}
  SECTION("reassigned ordinary source") {fixture.evaluate("@{type:Attachment} a={id:\"a\",child:{component:\"x\",anchor:\"a\"},target:{component:\"x\",anchor:\"a\"}};a=0;cube(1);");}
  SECTION("malformed metadata ordinary source") {fixture.evaluate("@{type:Attachment,unknown:1} a={id:\"a\"};cube(1);");}
  SECTION("Point endpoint") {fixture.evaluate("@{type:Part,id:\"p\"}module p(){@{type:Point} a={id:\"a\",position:[0,0,0]};cube(1);}@{type:Component,id:\"x\"}p();@{type:Component,id:\"y\"}p();@{type:Attachment} a={id:\"a\",child:{component:\"x\",anchor:\"a\"},target:{component:\"y\",anchor:\"a\"}};");}
  SECTION("two drivers") {fixture.evaluate(model()+"@{type:Attachment} another={id:\"another\",child:{component:\"/fixture-a/peg\",anchor:\"base\"},target:{component:\"/fixture-b/plate\",anchor:\"mount\"}};");}
  REQUIRE_FALSE(fixture.node);REQUIRE(fixture.diagnostic("attachment-invalid"));REQUIRE_FALSE(referenceScene(*fixture.scene));
}
TEST_CASE("Literal duplicate proof from a computed reference RHS is rejected without reevaluation", "[anchor-collection]") {
  int count=0;struct Restore {OutputHandlerFunc *handler;void *data;~Restore(){set_output_handler(handler,nullptr,data);}} restore{outputhandler,outputhandler_data};
  set_output_handler([](const Message& message,void *data){if(message.msg.find("ANCHOR_ONCE")!=std::string::npos)++*static_cast<int *>(data);},nullptr,&count);
  SemanticTest::Fixture fixture;fixture.evaluate("function make()=echo(\"ANCHOR_ONCE\") {id:\"a\",position:[0,0,0],position:[1,0,0],normal:[0,0,1],up:[0,1,0]};@{type:Anchor} a=make();cube(1);");
  REQUIRE(count==1);
  REQUIRE(fixture.node);auto scene=referenceScene(*fixture.scene);REQUIRE(scene->references.empty());REQUIRE(diagnostic(*scene,"anchor-schema"));
}
