#include "core/anchors/AnchorSourceEdits.h"
#include "core/design/DesignKinds.h"
#include "core/semantic/SemanticTestFixture.h"
#include <sstream>
using namespace Anchors;
namespace {
LiteralChange change(std::string path,Value value){return {std::move(path),std::make_shared<const Value>(Design::ownedValue(value))};}
Value vector(std::initializer_list<double> values){VectorType out(nullptr);for(double value:values)out.emplace_back(value);return Value(std::move(out));}
std::string applyPatches(std::string text,const std::vector<Design::SourcePatch>& patches){for(const auto& patch:patches){REQUIRE(text.substr(patch.range.begin,patch.range.end-patch.range.begin)==patch.expected);text.replace(patch.range.begin,patch.range.end-patch.range.begin,patch.replacement);}return text;}
std::shared_ptr<const Design::Record> record(SemanticTest::Fixture& fixture,const std::string& id="mount",const std::string& path=""){auto references=referenceScene(*fixture.scene);REQUIRE(references);return references->references.at({path.empty()?OwnerKey{OwnerRole::Document,fixture.inputs->root().fileKey}:instanceOwner(path,""),id}).declaration;}
const std::string source="width=40;\n@{type:Anchor} mount={id:\"mount\",label:\"日本語\",position:[width/2,0,10],normal:[0,0,1],up:[0,1,0]};\ncube([width,20,10]);\n";
}
TEST_CASE("Anchor literal source patches preserve parametric expressions UTF8 comments and line endings", "[anchor-source-edits]") {
  for(bool crlf:{false,true}) {
    auto text=source;if(crlf){std::string converted;for(char c:text){if(c=='\n')converted+='\r';converted+=c;}text=converted;}
    text.insert(text.find("position:"),"/* retained */ ");SemanticTest::Fixture fixture;fixture.evaluate(text);REQUIRE(fixture.node);auto before=record(fixture);std::string error;
    auto changes=std::vector<LiteralChange>{change("label",Value("New \"label\"")),change("adjust.translation",vector({1,2,3})),change("adjust.rotation",vector({0,0,1,1}))};
    auto patches=planReferenceEdit(*before,fixture.inputs->root(),changes,1,false,error);REQUIRE(patches);auto result=applyPatches(text,*patches);REQUIRE(result.find("width/2")!=std::string::npos);REQUIRE(result.find("/* retained */")!=std::string::npos);
    SemanticTest::Fixture after;after.evaluate(result);REQUIRE(after.node);auto decoded=std::dynamic_pointer_cast<const ReferenceDeclaration>(record(after)->evaluated);REQUIRE(std::get<Anchor>(decoded->value).translation.isApprox(Vector3d(1,2,3)));
    REQUIRE(before->label=="日本語");REQUIRE_FALSE(before->definition.toObject().contains("adjust"));
    if(crlf)for(size_t i=0;i<result.size();++i)if(result[i]=='\n')REQUIRE((i>0 && result[i-1]=='\r'));
  }
}
TEST_CASE("Shared reference declarations require explicit counted authorization before a source plan", "[anchor-source-edits]") {
  SemanticTest::Fixture fixture;fixture.evaluate("@{type:Part,id:\"part\"}module part(){@{type:Anchor} mount={id:\"mount\",position:[0,0,0],normal:[0,0,1],up:[0,1,0]};cube(1);}@{type:Component,id:\"a\"}part();@{type:Component,id:\"b\"}part();");REQUIRE(fixture.node);auto before=record(fixture,"mount","/a");auto uses=referenceScene(*fixture.scene)->sourceUseCounts.at(sourceIdentity(*before->source));REQUIRE(uses==2);std::string error;
  auto changes=std::vector<LiteralChange>{change("label",Value("Shared"))};REQUIRE_FALSE(planReferenceEdit(*before,fixture.inputs->root(),changes,uses,false,error));REQUIRE(error.find("2 invocations")!=std::string::npos);REQUIRE(planReferenceEdit(*before,fixture.inputs->root(),changes,uses,true,error));
}
TEST_CASE("Reference edits reject identity base fields helper RHS adjustment expressions and invalid values atomically", "[anchor-source-edits]") {
  SemanticTest::Fixture fixture;fixture.evaluate(source);auto before=record(fixture);std::string error;
  for(auto changes: {std::vector<LiteralChange>{change("id",Value("other"))},std::vector<LiteralChange>{change("position",vector({1,2,3}))},std::vector<LiteralChange>{change("adjust.rotation",vector({0,0,0,0}))}})
    REQUIRE_FALSE(planReferenceEdit(*before,fixture.inputs->root(),changes,1,false,error));
  auto expression=source;expression.insert(expression.find("};"),",adjust:{translation:[0,0,width/2]}");SemanticTest::Fixture computed;computed.evaluate(expression);auto patches=planReferenceEdit(*record(computed),computed.inputs->root(),{change("label",Value("Must not apply")),change("adjust.translation",vector({1,2,3}))},1,false,error);REQUIRE_FALSE(patches);REQUIRE(error.find("expression")!=std::string::npos);
  SemanticTest::Fixture helper;helper.evaluate("function helper()={id:\"mount\",position:[0,0,0],normal:[0,0,1],up:[0,1,0]};@{type:Anchor} mount=helper();cube(1);");REQUIRE_FALSE(planReferenceEdit(*record(helper),helper.inputs->root(),{change("label",Value("Source only"))},1,false,error));
  auto stale=fixture.inputs->root();stale.fingerprint="changed";REQUIRE_FALSE(planReferenceEdit(*before,stale,{change("label",Value("Stale"))},1,false,error));
}
TEST_CASE("Attachment edits preserve base offset spin expressions while changing only literal adjustments and mode", "[anchor-source-edits]") {
  SemanticTest::Fixture fixture;std::ifstream file(std::filesystem::path(OPENSCAD_TEST_DATA_DIR)/"scad/anchors/reusable-fixture.scad");std::ostringstream out;out<<file.rdbuf();auto text=out.str();auto offset=text.find("offset: [0,0,0]");REQUIRE(offset!=std::string::npos);text.replace(offset,std::string("offset: [0,0,0]").size(),"offset: [0,0,width/4]");fixture.evaluate(text);REQUIRE(fixture.node);
  auto relation=referenceScene(*fixture.scene)->attachments.begin()->second;std::string error;auto patches=planReferenceEdit(*relation.declaration,fixture.inputs->root(),{change("adjust.offset",vector({0,0,5})),change("adjust.spin",Value(15.)),change("mode",Value("align"))},2,true,error);REQUIRE(patches);auto result=applyPatches(text,*patches);REQUIRE(result.find("width/4")!=std::string::npos);REQUIRE(result.find("spin: 0")!=std::string::npos);
  SemanticTest::Fixture after;after.evaluate(result);REQUIRE(after.node);auto edited=referenceScene(*after.scene)->attachments.begin()->second;REQUIRE(edited.value.mode==AttachmentMode::Align);REQUIRE(edited.value.offsetAdjustment.z()==5);REQUIRE(edited.value.spinAdjustment==15);
}
TEST_CASE("Fixed capture serialization roundtrips exact local values stable identity and explicit escaped ownership", "[anchor-source-edits]") {
  registerKinds();Anchor anchor;anchor.fixedCapture=true;anchor.position=Vector3d(1.2345678901234567,-2,3);anchor.normal=Vector3d(1,2,3).normalized();anchor.up=Vector3d(0,3,-2).normalized();
  auto definition=capturedDefinition("stable","Fixed \"日本語\"",anchor,"/日本語~1part");std::vector<Design::Diagnostic> diagnostics;auto decoded=Design::makeRecord(*Design::kind("Anchor"),definition,{},diagnostics);REQUIRE(decoded);
  auto unsourced=std::make_shared<Design::Record>();unsourced->id=decoded->id;unsourced->kind=decoded->kind;unsourced->label=decoded->label;unsourced->definition=Design::ownedValue(decoded->definition);unsourced->evaluated=decoded->evaluated;
  auto serialized=serializeReference(*unsourced,"anchor_stable");REQUIRE(serialized);REQUIRE_FALSE(serializeReference(*unsourced,"bad;cube(100)"));
  SemanticTest::Fixture fixture;fixture.evaluate("@{type:Part,id:\"part\"}module part(){cube(1);}@{type:Component,id:\"日本語/part\"}part();"+*serialized);REQUIRE(fixture.node);auto retained=record(fixture,"stable","/日本語~1part");auto value=std::get<Anchor>(std::dynamic_pointer_cast<const ReferenceDeclaration>(retained->evaluated)->value);REQUIRE(value.position.x()==anchor.position.x());REQUIRE(value.normal.isApprox(anchor.normal,1e-15));REQUIRE(value.up.isApprox(anchor.up,1e-15));REQUIRE(value.fixedCapture);REQUIRE(retained->label=="Fixed \"日本語\"");
}
