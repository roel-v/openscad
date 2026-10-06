// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/CatalogLoader.h"
#include "core/providers/ContentDigest.h"
#include "core/providers/LibraryResolution.h"
#include "core/providers/ProviderRegistry.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
using namespace Providers;
using Json=nlohmann::json;
std::string path(const std::filesystem::path& p) { auto s=p.generic_u8string(); return {s.begin(),s.end()}; }
struct Fixture {
  std::string base=path(std::filesystem::temp_directory_path()/"provider-match-fixture");
  std::string root=base+"/libraries/Fixture";
  std::shared_ptr<std::map<std::string,SourceBytes>> files=std::make_shared<std::map<std::string,SourceBytes>>();
  std::shared_ptr<const RegistrySnapshot> snapshot;
  LibraryResolutionContext context;
  std::string id="fixture.library:cube";
  Fixture(const std::string& suffix="") {
    base+=suffix;root=base+"/libraries/Fixture";
    std::map<std::string,std::string> sources{{"a.scad","include <nested/detail.scad>\nmodule fixture_part(size,center) { cube(size,center); }\n"},
                                            {"nested/detail.scad","// supporting dependency\n"}};
    std::map<std::string,std::string> hashes;
    Json fileHashes=Json::array();
    for(const auto& [relative,bytes]:sources) {
      hashes[relative]=sha256(bytes); fileHashes.push_back({{"path",relative},{"sha256",hashes[relative]}});
      (*files)[root+"/"+relative]={bytes,"revision-1"};
    }
    std::ifstream in(std::filesystem::path(OPENSCAD_PROVIDER_TEST_DIR)/"builtin.json");
    auto json=Json::parse(in); json["providerId"]="fixture.library";
    json["libraries"]=Json::array({{{"libraryId","fixture-lib"},{"label","Fixture"},{"defaultImportPrefix","Fixture"},
      {"profiles",Json::array({{{"profileId","fixture-1"},{"releaseLabel","1.0.0"},{"revisionEvidence","fixture"},
      {"fileHashes",fileHashes},{"importEdges",Json::array({{{"from","a.scad"},{"kind","include"},{"import","nested/detail.scad"},{"to","nested/detail.scad"}}})},
      {"digest",closureDigest(hashes)}}})}}});
    json["items"]=Json::array({json["items"][0]}); auto& recipe=json["items"][0]["recipes"][0];
    recipe["profileIds"]={"fixture-1"}; recipe["imports"]=Json::array({{{"libraryId","fixture-lib"},{"path","a.scad"},{"kind","include"},{"scope","file"}}});
    recipe["moduleBindings"]=Json::array({{{"module","fixture_part"},{"libraryId","fixture-lib"},{"path","a.scad"}}});
    recipe["geometry"]["module"]="fixture_part";
    auto loaded=loadCatalog(json.dump(),"fixture.json"); REQUIRE(loaded);
    auto registry=std::make_shared<RegistrySnapshot>(); registry->generation=1; registry->providers.push_back(*loaded.value);
    registry->items[id]=loaded.value->items[0]; snapshot=registry;
    context.sourceDirectory=base+"/document"; context.revision="document-1"; context.searchPaths={base+"/libraries"};
    context.resolveImport=[data=files,paths=context.searchPaths](const std::string& from,const std::string& spelling)->Result<std::string> {
      auto candidate=path((std::filesystem::u8path(from)/std::filesystem::u8path(spelling)).lexically_normal());
      if(data->count(candidate)) return {candidate,{}};
      for(const auto& directory:paths) {
        candidate=path((std::filesystem::u8path(directory)/std::filesystem::u8path(spelling)).lexically_normal());
        if(data->count(candidate)) return {candidate,{}};
      }
      return {{},Diagnostic{"MissingLibrary",spelling,"","Not captured"}};
    };
    context.readBytes=[data=files](const std::string& canonical)->Result<SourceBytes> {
      auto it=data->find(canonical);
      if(it==data->end()) return {{},Diagnostic{"NotCaptured",canonical,"","Input is not captured"}};
      return {it->second,{}};
    };
  }
};
}
TEST_CASE("provider_binding_captures_complete_closure_and_revalidates", "[providers-match]")
{
  Fixture fixture;
  auto binding=ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context); REQUIRE(binding);
  REQUIRE(binding.value->libraries.size()==1); CHECK(binding.value->libraries[0].files.size()==2);
  CHECK(ProviderRegistry::revalidate(*binding.value,fixture.context,fixture.snapshot));
  fixture.files->at(fixture.root+"/nested/detail.scad").bytes+="// unsaved edit\n";
  auto invalid=ProviderRegistry::revalidate(*binding.value,fixture.context,fixture.snapshot); REQUIRE_FALSE(invalid);
  CHECK(invalid.diagnostic->code=="StaleBinding"); CHECK(invalid.diagnostic->firstMismatchingFile=="nested/detail.scad");
}
TEST_CASE("provider_matching_normalizes_line_endings_without_ignoring_other_edits", "[providers-match]")
{
  Fixture fixture;
  auto& data=fixture.files->at(fixture.root+"/nested/detail.scad"); data.bytes="\xef\xbb\xbf// supporting dependency\r\n";
  CHECK(ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context));
  data.bytes="// supporting dependency!\n";
  auto invalid=ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context); REQUIRE_FALSE(invalid);
  CHECK(invalid.diagnostic->code=="ModifiedLibrary"); CHECK(invalid.diagnostic->detectedHashes.size()==2);
}
TEST_CASE("provider_matching_rejects_shadow_imports_and_escaping_edges", "[providers-match]")
{
  Fixture fixture;
  fixture.context.libraryBindings["fixture-lib"]={fixture.root,"Fixture"};
  auto shadow=fixture.context.sourceDirectory+"/Fixture/a.scad";
  (*fixture.files)[shadow]={"// incompatible local shadow\n","shadow-1"};
  auto invalid=ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context); REQUIRE_FALSE(invalid);
  CHECK(invalid.diagnostic->code=="ResolutionMismatch"); CHECK(invalid.diagnostic->path==shadow);
  fixture.files->erase(shadow);
  auto resolver=fixture.context.resolveImport;
  fixture.context.resolveImport=[resolver,root=fixture.root](const std::string& from,const std::string& spelling)->Result<std::string> {
    if(from==root && spelling=="nested/detail.scad") return {root+"-outside/detail.scad",{}};
    return resolver(from,spelling);
  };
  invalid=ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context); REQUIRE_FALSE(invalid);
  CHECK(invalid.diagnostic->code=="ResolutionMismatch"); CHECK(invalid.diagnostic->message.find("edge")!=std::string::npos);
}
TEST_CASE("provider_binding_rejects_context_registry_and_capture_races", "[providers-match]")
{
  Fixture fixture; auto binding=ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context); REQUIRE(binding);
  auto newer=std::make_shared<RegistrySnapshot>(*fixture.snapshot); ++newer->generation;
  CHECK_FALSE(ProviderRegistry::revalidate(*binding.value,fixture.context,newer));
  fixture.context.searchPaths.push_back("extra-search-path");
  CHECK_FALSE(ProviderRegistry::revalidate(*binding.value,fixture.context,fixture.snapshot));
  fixture.context.searchPaths.pop_back();
  auto reader=fixture.context.readBytes;
  auto reads=std::make_shared<size_t>(0);
  fixture.context.readBytes=[reader,reads](const std::string& p)->Result<SourceBytes> {
    auto result=reader(p); if(++*reads>2 && result) result.value->bytes+="// changed during capture\n"; return result;
  };
  auto raced=ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context); REQUIRE_FALSE(raced);
  CHECK(raced.diagnostic->code=="StaleBinding");
  fixture.context.readBytes=[](const std::string& p)->Result<SourceBytes> {return {{},Diagnostic{"NotCaptured",p,"","No captured bytes"}};};
  CHECK_FALSE(ProviderRegistry::bind(fixture.snapshot,fixture.id,fixture.context));
}
TEST_CASE("provider_matching_ambiguous_recipes_are_never_selected", "[providers-match]")
{
  Fixture fixture; auto ambiguous=std::make_shared<RegistrySnapshot>(*fixture.snapshot);
  ambiguous->items.at(fixture.id).recipes.push_back(ambiguous->items.at(fixture.id).recipes[0]);
  auto result=ProviderRegistry::bind(ambiguous,fixture.id,fixture.context); REQUIRE_FALSE(result);
  CHECK(result.diagnostic->code=="AmbiguousProfile");
}
TEST_CASE("provider_native_resolver_preserves_relative_precedence_and_utf8_paths", "[providers-match]")
{
  Fixture fixture;
  auto directory=std::filesystem::temp_directory_path()/std::filesystem::u8path("provider-native-測試-é-"+
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(directory.is_absolute()); REQUIRE(std::filesystem::create_directory(directory));
  struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);} } cleanup{directory};
  std::filesystem::create_directories(directory/"Fixture"/"nested");
  for(const auto& [file,bytes]:*fixture.files) {
    auto relative=std::filesystem::u8path(file).lexically_relative(std::filesystem::u8path(fixture.root));
    std::ofstream out(directory/"Fixture"/relative,std::ios::binary); out<<bytes.bytes;
  }
  auto context=nativeResolutionContext(path(std::filesystem::canonical(directory))); REQUIRE(context);
  auto result=ProviderRegistry::bind(fixture.snapshot,fixture.id,*context.value); REQUIRE(result);
  CHECK(result.value->libraries[0].root==path(std::filesystem::canonical(directory/"Fixture")));
  auto nested=context.value->resolveImport(result.value->libraries[0].root,"nested/detail.scad"); REQUIRE(nested);
  CHECK(*nested.value==path(std::filesystem::canonical(directory/"Fixture"/"nested"/"detail.scad")));
}
TEST_CASE("provider_fake_consumer_preserves_immutable_drafts_across_documents", "[providers-match][providers-source]")
{
  Fixture first("-first"),second("-second");
  auto firstSnapshot=std::make_shared<RegistrySnapshot>(*first.snapshot),secondSnapshot=std::make_shared<RegistrySnapshot>(*second.snapshot);
  for(auto snapshot:{firstSnapshot,secondSnapshot}) {
    auto& recipe=snapshot->items.at(first.id).recipes[0];HelperDefinition helper;
    helper.id="part";helper.parameters={"width","depth","height"};helper.body=recipe.geometry;recipe.helpers.push_back(helper);
    recipe.geometry={"part",true,{},{}};
    for(const auto& name:helper.parameters){auto expression=std::make_shared<Expression>();expression->kind=Expression::Kind::Parameter;expression->name=name;recipe.geometry.arguments.emplace_back(name,expression);}
  }
  first.context.revision="document-first-1";second.context.revision="document-second-1";
  auto a=ProviderRegistry::bind(firstSnapshot,first.id,first.context),b=ProviderRegistry::bind(secondSnapshot,second.id,second.context);REQUIRE(a);REQUIRE(b);
  auto draftA=ProviderRegistry::generate(*a.value,{{"width",10.0}},{{"part","document_a_part"}});
  auto draftB=ProviderRegistry::generate(*b.value,{{"width",22.0}},{{"part","document_b_part"}});REQUIRE(draftA);REQUIRE(draftB);
  CHECK(draftA.value->helperSource.find("document_a_part")!=std::string::npos);CHECK(draftB.value->helperSource.find("document_b_part")!=std::string::npos);
  CHECK(draftA.value->frame.origin[0]==5);CHECK(draftB.value->frame.origin[0]==11);
  CHECK(a.value->libraries[0].root!=b.value->libraries[0].root);
  second.files->at(second.root+"/nested/detail.scad").bytes+="// edited other document's dependency\n";
  auto validA=ProviderRegistry::revalidate(draftA.value->binding,first.context,firstSnapshot);
  auto staleB=ProviderRegistry::revalidate(draftB.value->binding,second.context,secondSnapshot);REQUIRE(validA);REQUIRE_FALSE(staleB);
  CHECK(staleB.diagnostic->code=="StaleBinding");CHECK(draftB.value->binding.libraries[0].files[1].normalizedHash!=sha256(second.files->at(second.root+"/nested/detail.scad").bytes));
  CHECK(draftB.value->geometrySource.find("22")!=std::string::npos);
}
