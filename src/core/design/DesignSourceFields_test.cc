#include "core/design/DesignSourceFields.h"
#include <catch2/catch_test_macros.hpp>

namespace {
Design::DesignSourceMap scan(const std::string& bytes,const std::string& name="section") {
  SourceInput input{"session:test",bytes,EvaluationInputs::fingerprint(bytes),"test",3};
  return Design::DesignSourceMap::scan({input.fileKey,input.fingerprint,name,Design::DeclarationHost::Assignment,0,bytes.size()},input,"Section");
}
std::string applyPatches(std::string bytes,const std::vector<Design::SourcePatch>& patches) {
  for(const auto& patch:patches){REQUIRE(bytes.substr(patch.range.begin,patch.range.end-patch.range.begin)==patch.expected);bytes.replace(patch.range.begin,patch.range.end-patch.range.begin,patch.replacement);}return bytes;
}
}
TEST_CASE("Design field patches preserve parametric source trivia Unicode and trailing comma style", "[design-source-edits]") {
  for(const std::string eol:{"\n","\r\n"}){
    std::string bytes="@{type: Section}"+eol+"section = {"+eol+"  id: \"stable\", // original"+eol+"  label: \"日本語 \\\"label\\\"\","+eol+"  origin: [0, 0, height/2],"+eol+"  normal: [0,0,1],"+eol+"  adjust: {offset: -2, /* keep */ rotation: [0,0,0,1],},"+eol+"};";
    auto map=scan(bytes);REQUIRE(map.direct());std::string error;
    auto patches=map.patches({{"label","\"New\"",Design::LiteralKind::String},{"adjust.offset","7",Design::LiteralKind::Number},{"adjust.translation","[1,2,3]",Design::LiteralKind::Vector3}},error);REQUIRE(patches);
    auto changed=applyPatches(bytes,*patches);REQUIRE(changed.find("height/2")!=std::string::npos);REQUIRE(changed.find("/* keep */")!=std::string::npos);REQUIRE(changed.find("// original")!=std::string::npos);
    REQUIRE(changed.find("offset: 7")!=std::string::npos);REQUIRE(scan(changed).direct());
    if(eol=="\r\n")for(size_t i=0;i<changed.size();++i)if(changed[i]=='\n')REQUIRE((i>0 && changed[i-1]=='\r'));
  }
}
TEST_CASE("Design source scanner recognizes fixed string keys and nested delimiters without flattening expressions", "[design-source-edits]") {
  std::string bytes="@{type: Section} section={\"id\":\"stable\", \"label\":\"first\nsecond\",origin:[0,0, helper({v:[2,3]})/2],normal:[0,0,1],adjust:{\"offset\":1}};";
  auto map=scan(bytes);REQUIRE(map.direct());std::string error;auto patches=map.patches({{"adjust.offset","2",Design::LiteralKind::Number}},error);REQUIRE(patches);
  REQUIRE(applyPatches(bytes,*patches).find("helper({v:[2,3]})/2")!=std::string::npos);
}
TEST_CASE("Design helper computed duplicate and truncated declarations report source-only editing", "[design-source-edits]") {
  for(const std::string rhs:{"helper()","{[key]:1}","{id:\"a\",id:\"b\"}","{each helper()}","{id:\"a\",adjust: helper()}"}){
    auto map=scan("@{type: Section} section="+rhs+";");REQUIRE_FALSE(map.direct());REQUIRE(map.ownsDeclaration());std::string error;REQUIRE_FALSE(map.patches({{"adjust.offset","3",Design::LiteralKind::Number}},error));REQUIRE(map.deletion(error));
  }
  for(const std::string bytes:{"@{type: Section} section={id:\"x\"}","@{type: Section} other={};","@{type: Section} section={id:\"unfinished};","@{type: Section} section={}; unrelated=1;"}){
    auto map=scan(bytes);REQUIRE_FALSE(map.direct());std::string error;REQUIRE_FALSE(map.deletion(error));
  }
}
TEST_CASE("Design numerical edits refuse adjustment expressions and merge repeated field changes", "[design-source-edits]") {
  auto map=scan("@{type: Section} section={id:\"x\",origin:[0,0,height/2],normal:[0,0,1],adjust:{offset:height/4}};");REQUIRE(map.direct());std::string error;
  REQUIRE_FALSE(map.patches({{"adjust.offset","3",Design::LiteralKind::Number}},error));REQUIRE(error.find("expression")!=std::string::npos);
  std::string bytes="@{type: Section} section={id:\"x\",origin:[0,0,height/2],normal:[0,0,1]};";map=scan(bytes);
  auto patches=map.patches({{"adjust.offset","2",Design::LiteralKind::Number},{"adjust.offset","3",Design::LiteralKind::Number}},error);REQUIRE(patches);
  REQUIRE(applyPatches(bytes,*patches).find("adjust: {offset: 3}")!=std::string::npos);
}
TEST_CASE("Design insertions preserve multiline comma and comment ownership", "[design-source-edits]") {
  for(const std::string suffix:{",\n}"," // retained\n}","\n}"}){
    std::string bytes="@{type: Section}\nsection={\n  id:\"x\",\n  origin:[0,0,height/2],\n  normal:[0,0,1]"+suffix+";";
    auto map=scan(bytes);REQUIRE(map.direct());std::string error;auto patches=map.patches({{"adjust.offset","5",Design::LiteralKind::Number}},error);REQUIRE(patches);auto changed=applyPatches(bytes,*patches);REQUIRE(scan(changed).direct());
    if(suffix.find("retained")!=std::string::npos)REQUIRE(changed.find("// retained")!=std::string::npos);
  }
}
