// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/CatalogLoader.h"
#include "core/providers/ContentDigest.h"
#include "core/providers/ProviderRegistry.h"
#include "core/providers/ScadFragmentWriter.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/EvaluationSession.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "geometry/GeometryEvaluator.h"
#include "openscad.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>

namespace {
using namespace Providers;
using Json=nlohmann::json;
std::string builtinBytes()
{
  std::ifstream in(std::filesystem::path(OPENSCAD_PROVIDER_TEST_DIR)/"builtin.json",std::ios::binary);
  std::ostringstream out; out<<in.rdbuf(); return out.str();
}
Json builtinJson() { return Json::parse(builtinBytes()); }
ProviderBinding binding(const std::string& id)
{
  auto loaded=loadCatalog(builtinBytes(),"fixture.json"); REQUIRE(loaded);
  auto snapshot=std::make_shared<RegistrySnapshot>(); snapshot->generation=1;
  for(const auto& item:loaded.value->items) snapshot->items.emplace(item.qualifiedId,item);
  auto key="scadmaxx.builtin:"+id;
  return {snapshot,key,snapshot->items.at(key).catalogDigest,"","",0,{}};
}
}
TEST_CASE("provider_duplicate_json_keys_keep_pointer", "[providers-schema]")
{
  auto result=parseJson("{\"items\":[{\"id\":\"a\",\"id\":\"b\"}]}","duplicate.json");
  REQUIRE_FALSE(result); CHECK(result.diagnostic->pointer=="/items/0/id"); CHECK(result.diagnostic->path=="duplicate.json");
}
TEST_CASE("provider_schema_rejects_unknown_fields_and_roles", "[providers-schema]")
{
  auto json=builtinJson(); json["items"][0]["insertionRole"]="component";
  CHECK(loadCatalog(json.dump(),"role.json"));
  json["items"][0]["operation"]="subtractive"; CHECK_FALSE(loadCatalog(json.dump(),"role.json"));
  json=builtinJson(); json["items"][0]["insertionRole"]="hardware"; CHECK_FALSE(loadCatalog(json.dump(),"role.json"));
  json=builtinJson(); json["items"][0]["extra"]=1; auto invalid=loadCatalog(json.dump(),"unknown.json");
  REQUIRE_FALSE(invalid); CHECK(invalid.diagnostic->pointer=="/items/0/extra");
  json=builtinJson(); json["items"][0].erase("insertionRole"); auto loaded=loadCatalog(json.dump(),"defaults.json");
  REQUIRE(loaded); CHECK(loaded.value->items[0].insertionRole==InsertionRole::Geometry);
}
TEST_CASE("provider_parameters_validate_bounds_types_and_defaults", "[providers-schema]")
{
  auto b=binding("cube");
  CHECK_FALSE(ProviderRegistry::generate(b,{{"width",0.0}},{}));
  CHECK_FALSE(ProviderRegistry::generate(b,{{"width",100001.0}},{}));
  CHECK_FALSE(ProviderRegistry::generate(b,{{"width",std::numeric_limits<double>::infinity()}},{}));
  CHECK_FALSE(ProviderRegistry::generate(b,{{"width",std::string("10")}},{}));
  CHECK_FALSE(ProviderRegistry::generate(b,{{"other",5.0}},{}));
  CHECK(ProviderRegistry::generate(b,{{"width",0.0001}},{}));
  auto json=builtinJson(); json["items"][0]["parameters"][0]["default"]=-1;
  CHECK_FALSE(loadCatalog(json.dump(),"default.json"));
}
TEST_CASE("provider_builtin_source_and_contact_are_deterministic", "[providers-source]")
{
  auto b=binding("cube"); auto a=ProviderRegistry::generate(b,{},{}), explicitValues=ProviderRegistry::generate(b,{{"width",10.0},{"depth",20.0},{"height",30.0}},{});
  REQUIRE(a); REQUIRE(explicitValues); CHECK(a.value->standaloneSource==explicitValues.value->standaloneSource);
  CHECK(a.value->geometrySource=="cube(size = [10, 20, 30], center = false);\n");
  CHECK(a.value->frame.origin==std::array<double,3>{5,10,0});
  CHECK(a.value->frame.normal==std::array<double,3>{0,0,-1});
  auto hole=ProviderRegistry::generate(binding("plain-hole"),{},{}); REQUIRE(hole);
  CHECK(hole.value->operation==Operation::Subtractive); CHECK(hole.value->frame.normal[2]==1);
}
TEST_CASE("provider_arithmetic_and_degenerate_frames_fail_atomically", "[providers-source]")
{
  auto json=builtinJson(); json["items"][0]["recipes"][0]["placement"]["origin"][0]["arguments"][1]["constant"]=0;
  auto loaded=loadCatalog(json.dump(),"division.json"); REQUIRE_FALSE(loaded);
  CHECK(loaded.diagnostic->message=="Division by zero");
  json=builtinJson(); json["items"][0]["recipes"][0]["placement"]["up"]=json["items"][0]["recipes"][0]["placement"]["normal"];
  CHECK_FALSE(loadCatalog(json.dump(),"parallel.json"));
}
TEST_CASE("provider_literals_escape_strings_and_ignore_decimal_locale", "[providers-source]")
{
  CHECK(scalarSource(std::string("a\"; cube(100);\\\n\t"))=="\"a\\\"; cube(100);\\\\\\n\\t\"");
  CHECK(scalarSource(-0.0)=="0");
  CHECK_THROWS(scalarSource(std::string("a\0b",3)));
  CHECK_THROWS(scalarSource(std::numeric_limits<double>::quiet_NaN()));
  struct Comma : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
  struct RestoreLocale { std::locale previous=std::locale(); ~RestoreLocale(){std::locale::global(previous);} } restore;
  std::locale::global(std::locale(std::locale::classic(),new Comma));
  CHECK(scalarSource(1.5)=="1.5");
}
TEST_CASE("provider_hash_normalization_and_standard_vectors", "[providers-schema]")
{
  CHECK(sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto result=normalizedUtf8("\xef\xbb\xbf" "a\r\nb\rc\n","utf8.scad"); REQUIRE(result); CHECK(*result.value=="a\nb\nc\n");
  CHECK_FALSE(normalizedUtf8(std::string("\xc0\xaf",2),"invalid.scad"));
  auto doubled=normalizedUtf8("\xef\xbb\xbf" "\xef\xbb\xbf" "x","double-bom.scad"); REQUIRE(doubled);
  CHECK(*doubled.value=="\xef\xbb\xbf" "x");
}
TEST_CASE("provider_generated_builtins_evaluate_with_native_geometry", "[providers-source]")
{
  Builtins::initialize();
  const std::vector<std::tuple<std::string,Vector3d,Vector3d>> expected{
    {"cube",{0,0,0},{10,20,30}}, {"sphere",{-5,-5,-5},{5,5,5}},
    {"cylinder",{-5,-5,0},{5,5,20}}, {"plain-hole",{-2,-2,0},{2,2,12}}};
  for (const auto& [id,minimum,maximum] : expected) {
    INFO(id);
    auto fragment=ProviderRegistry::generate(binding(id),{},{}); REQUIRE(fragment);
    SourceFile *raw=nullptr;
    REQUIRE(parse(raw,"$fn=64;\n"+fragment.value->standaloneSource,"provider-native.scad","provider-native.scad",0));
    std::unique_ptr<SourceFile> source(raw);
    EvaluationSession session(".");
    ContextHandle<BuiltinContext> context{Context::create<BuiltinContext>(&session)};
    std::shared_ptr<const FileContext> fileContext;
    auto root=source->instantiate(*context,&fileContext); REQUIRE(fileContext); REQUIRE(root);
    Tree tree(root,"."); GeometryEvaluator evaluator(tree);
    auto geometry=evaluator.evaluateGeometry(*root,false); REQUIRE(geometry); REQUIRE_FALSE(geometry->isEmpty());
    auto box=geometry->getBoundingBox();
    CHECK((box.min()-minimum).norm()<0.03);
    CHECK((box.max()-maximum).norm()<0.03);
  }
}
