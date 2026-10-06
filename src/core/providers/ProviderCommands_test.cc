// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/ProviderCommands.h"
#include "core/providers/ContentDigest.h"
#include "core/providers/ProviderRegistry.h"
#include "core/providers/LibraryResolution.h"
#include "core/SourceFile.h"
#include "core/UserModule.h"
#include "openscad.h"
#include "ext/json/json.hpp"
#include <catch2/catch_test_macros.hpp>
#include <QProcess>
#include <QProcessEnvironment>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
using namespace Providers;using Json=nlohmann::json;
std::string path(const std::filesystem::path& p){auto s=p.generic_u8string();return {s.begin(),s.end()};}
std::string read(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);std::ostringstream out;out<<in.rdbuf();REQUIRE(in);return out.str();}
void write(const std::filesystem::path& p,const std::string& bytes){std::ofstream out(p,std::ios::binary);out<<bytes;REQUIRE(out);}
struct Temporary {
  std::filesystem::path directory=std::filesystem::temp_directory_path()/std::filesystem::u8path("provider-cli-測試-é "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temporary(){REQUIRE(directory.is_absolute());REQUIRE(std::filesystem::create_directory(directory));}
  ~Temporary(){std::error_code error;std::filesystem::remove_all(directory,error);}
};
struct Response{int code;Json json;std::string stderrText;};
Response command(const std::filesystem::path& directory,const QStringList& arguments)
{
  QProcess process;process.setWorkingDirectory(QString::fromStdString(path(directory)));
  auto environment=QProcessEnvironment::systemEnvironment();environment.remove("OPENSCADPATH");process.setProcessEnvironment(environment);
  process.start(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE),arguments);
  REQUIRE(process.waitForStarted());REQUIRE(process.waitForFinished(15000));REQUIRE(process.exitStatus()==QProcess::NormalExit);
  auto bytes=process.readAllStandardOutput().toStdString(),errors=process.readAllStandardError().toStdString();INFO(bytes);INFO(errors);
  REQUIRE(Json::accept(bytes));return {process.exitCode(),Json::parse(bytes),errors};
}
const Json& item(const Json& report,const std::string& id)
{for(const auto& row:report.at("items"))if(row.at("qualifiedId")==id)return row;FAIL("Missing item: "+id);return report;}
}
TEST_CASE("provider_cli_native_list_emit_and_usage_do_not_open_gui", "[providers-cli]")
{
  Temporary temporary;
  auto list=command(temporary.directory,{"--provider-list"});INFO(list.json.dump());REQUIRE(list.code==0);
  CHECK(list.json["sourceContext"]=="cwd");CHECK(item(list.json,"scadmaxx.builtin:cube")["availability"]=="Available");
  auto a=command(temporary.directory,{"--provider-emit","scadmaxx.builtin:cube"});REQUIRE(a.code==0);
  auto b=command(temporary.directory,{"--provider-emit","scadmaxx.builtin:cube"});REQUIRE(b.code==0);
  CHECK(a.json["fragment"]["standaloneSource"]==b.json["fragment"]["standaloneSource"]);
  CHECK(a.json["fragment"]["frame"]["origin"]==Json::array({5,10,0}));
  auto invalid=command(temporary.directory,{"--provider-emit","unknown:item"});CHECK(invalid.code==1);CHECK_FALSE(invalid.json.contains("fragment"));
  for(const auto& args:std::vector<QStringList>{{"--provider-list","--provider-emit","scadmaxx.builtin:cube"},
      {"--provider-list","-o","output.stl"},{"--provider-list","--animate","3"},
      {"--provider-values","values.json"},{"--provider-list","--provider-values","values.json"},{"--provider-unknown"}})
    CHECK(command(temporary.directory,args).code==2);
  CHECK_FALSE(std::filesystem::exists(temporary.directory/"output.stl"));
}
TEST_CASE("provider_cli_project_wrapper_revision_and_duplicate_rules", "[providers-cli]")
{
  Temporary temporary;auto project=temporary.directory/std::filesystem::u8path("Project α spaced");
  std::filesystem::create_directories(project/".scadmaxx"/"providers");std::filesystem::create_directory(project/"MyParts");
  auto example=std::filesystem::path(OPENSCAD_PROVIDER_TEST_DIR).parent_path()/"examples"/"providers";
  auto catalog=project/".scadmaxx"/"providers"/"catalog.json",wrapper=project/"MyParts"/"bracket.scad",source=project/"part model.scad";
  std::filesystem::copy_file(example/"project-provider.json",catalog);std::filesystem::copy_file(example/"MyParts"/"bracket.scad",wrapper);
  write(source,"assert(false, \"provider commands must not evaluate this source\");\n");auto original=read(source);
  auto target=QString::fromStdString(path(source));auto checkPath=QString::fromStdString(path(catalog));
  auto list=command(project,{"--provider-list","--provider-source",target});INFO(list.json.dump());REQUIRE(list.code==0);
  CHECK(item(list.json,"example.my-parts:bracket")["availability"]=="Available");
  auto valuesFile=project/"values.json";write(valuesFile,Json{{"width",30},{"label","測試 \"; cube(999); //"},{"segments",48}}.dump());
  auto emitted=command(project,{"--provider-emit","example.my-parts:bracket","--provider-source",target,"--provider-values",QString::fromStdString(path(valuesFile))});REQUIRE(emitted.code==0);
  CHECK(emitted.json["fragment"]["parameters"]["label"]=="測試 \"; cube(999); //");CHECK(emitted.json["fragment"]["frame"]["origin"]==Json::array({15,6,0}));
  CHECK(read(source)==original);
  auto generated=project/"generated source.scad",output=project/"generated mesh.stl",summary=project/"summary.json";
  write(generated,emitted.json["fragment"]["standaloneSource"].get<std::string>());
  QProcess native;native.setWorkingDirectory(QString::fromStdString(path(project)));
  native.start(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE),QStringList{"--backend","Manifold","--hardwarnings","--summary","all","--summary-file",QString::fromStdString(path(summary)),"-o",QString::fromStdString(path(output)),QString::fromStdString(path(generated))});
  REQUIRE(native.waitForStarted());REQUIRE(native.waitForFinished(15000));INFO(native.readAllStandardError().toStdString());REQUIRE(native.exitCode()==0);
  auto geometry=Json::parse(read(summary));CHECK(geometry["geometry"]["bounding_box"]["size"]==Json::array({30,12,5}));
  write(valuesFile,"{\"width\":\"30\"}");auto badValues=command(project,{"--provider-emit","example.my-parts:bracket","--provider-values",QString::fromStdString(path(valuesFile))});CHECK(badValues.code==1);CHECK_FALSE(badValues.json.contains("fragment"));
  write(wrapper,read(wrapper)+"// explicit authored revision\n");
  auto mismatch=command(project,{"--provider-check",checkPath,"--provider-source",target});CHECK(mismatch.code==1);
  CHECK(item(mismatch.json,"example.my-parts:bracket")["availability"]=="ModifiedLibrary");
  auto changed=Json::parse(read(catalog));auto bytes=normalizedUtf8(read(wrapper),path(wrapper));REQUIRE(bytes);
  auto hash=sha256(*bytes.value);changed["providerVersion"]="1.0.1";changed["libraries"][0]["profiles"][0]["fileHashes"][0]["sha256"]=hash;
  changed["libraries"][0]["profiles"][0]["digest"]=closureDigest({{"bracket.scad",hash}});write(catalog,changed.dump());
  CHECK(command(project,{"--provider-check",checkPath,"--provider-source",target}).code==0);
  std::filesystem::copy_file(catalog,catalog.parent_path()/"duplicate.json");
  CHECK(command(project,{"--provider-check",checkPath,"--provider-source",target}).code==1);
  auto duplicate=command(project,{"--provider-list","--provider-source",target});CHECK(item(duplicate.json,"example.my-parts:bracket")["availability"]=="DuplicateCatalog");
}
TEST_CASE("provider_discovery_configuration_is_atomic_and_does_not_search_ancestors", "[providers-cli]")
{
  Temporary temporary;auto user=temporary.directory/"user",project=temporary.directory/"project",child=project/"child";
  std::filesystem::create_directories(user);std::filesystem::create_directories(child);
  auto config=user/"provider-sources.json";
  write(config,Json{{"schemaVersion",1},{"catalogPaths",Json::array({"../custom-catalogs"})},{"libraryBindings",Json::array({{{"libraryId","my-parts"},{"root","../libraries/MyParts"},{"importPrefix","MyParts"}}})}}.dump());
  auto result=discoverProviders({OPENSCAD_PROVIDER_TEST_DIR,path(user),path(child),{}});REQUIRE(result.diagnostics.empty());
  CHECK(result.libraryBindings.at("my-parts").root==path(std::filesystem::weakly_canonical(temporary.directory/"libraries"/"MyParts")));
  CHECK(std::find(result.catalogs.paths.begin(),result.catalogs.paths.end(),path(project/".scadmaxx"/"providers"))==result.catalogs.paths.end());
  write(config,"{\"schemaVersion\":1,\"schemaVersion\":2}");result=discoverProviders({OPENSCAD_PROVIDER_TEST_DIR,path(user),path(child),{}});
  REQUIRE(result.diagnostics.size()==1);CHECK(result.diagnostics[0].pointer=="/schemaVersion");CHECK(result.libraryBindings.empty());
  auto invalid=temporary.directory/"invalid.json";write(invalid,"{\"schemaVersion\":99}");
  std::ostringstream output;ProviderCommand command;command.mode=ProviderCommand::Mode::List;command.extraCatalogs={path(invalid)};
  CHECK(runProviderCommand(command,{OPENSCAD_PROVIDER_TEST_DIR,path(user),path(child)},output)==0);
  auto report=Json::parse(output.str());CHECK(item(report,"scadmaxx.builtin:cube")["availability"]=="Available");CHECK(report["diagnostics"].size()>=2);
}
TEST_CASE("provider_consumer_module_identity_detects_a_model_override", "[providers-source]")
{
  auto example=std::filesystem::path(OPENSCAD_PROVIDER_TEST_DIR).parent_path()/"examples"/"providers";
  auto snapshot=ProviderRegistry::load({{path(example/"project-provider.json")}});REQUIRE(snapshot->diagnostics.empty());
  auto context=nativeResolutionContext(path(std::filesystem::canonical(example)));REQUIRE(context);
  auto binding=ProviderRegistry::bind(snapshot,"example.my-parts:bracket",*context.value);REQUIRE(binding);
  auto fragment=ProviderRegistry::generate(*binding.value,{},{});REQUIRE(fragment);REQUIRE(fragment.value->moduleBindings.size()==1);
  auto model=path(example/"collision-model.scad");SourceFile *raw=nullptr;
  REQUIRE(parse(raw,fragment.value->standaloneSource+"\nmodule bracket() { cube(999); }\n",model,model,0));std::unique_ptr<SourceFile> parsed(raw);
  auto declared=parsed->scope->getUserModules().find(fragment.value->moduleBindings[0].module);REQUIRE(declared!=parsed->scope->getUserModules().end());
  auto expected=path(std::filesystem::canonical(example/"MyParts"/"bracket.scad"));
  CHECK(declared->second->location().fileName()!=expected);CHECK(declared->second->location().fileName()==model);
}
