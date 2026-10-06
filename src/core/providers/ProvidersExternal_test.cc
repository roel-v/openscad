// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/ProviderRegistry.h"
#include "core/providers/LibraryResolution.h"
#include "core/providers/ContentDigest.h"
#include "core/parsersettings.h"
#include "core/SourceFile.h"
#include "core/UserModule.h"
#include "platform/PlatformUtils.h"
#include "openscad.h"
#include "ext/json/json.hpp"
#include <catch2/catch_test_macros.hpp>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace {
using namespace Providers;
std::string utf8(const std::filesystem::path& p) {auto s=p.generic_u8string();return {s.begin(),s.end()};}
std::string read(const std::filesystem::path& p) {std::ifstream in(p,std::ios::binary);std::ostringstream out;out<<in.rdbuf();REQUIRE(in);return out.str();}
void write(const std::filesystem::path& p,const std::string& bytes) {std::ofstream out(p,std::ios::binary);out<<bytes;REQUIRE(out);}
using Vector=std::array<double,3>;
struct Mesh {Vector min,max;double volume=0;size_t triangles=0;};
Mesh mesh(const std::filesystem::path& file)
{
  Mesh out{{INFINITY,INFINITY,INFINITY},{-INFINITY,-INFINITY,-INFINITY}};
  std::istringstream input(read(file)); input.imbue(std::locale::classic());
  std::string token; std::array<Vector,3> vertices;size_t index=0;
  while(input>>token) if(token=="vertex") {
    auto& v=vertices[index++%3];REQUIRE(input>>v[0]>>v[1]>>v[2]);
    for(size_t axis=0;axis<3;++axis) {REQUIRE(std::isfinite(v[axis]));out.min[axis]=std::min(out.min[axis],v[axis]);out.max[axis]=std::max(out.max[axis],v[axis]);}
    if(index%3==0) {const auto& a=vertices[0];const auto& b=vertices[1];const auto& c=vertices[2];
      out.volume+=(a[0]*(b[1]*c[2]-b[2]*c[1])+a[1]*(b[2]*c[0]-b[0]*c[2])+a[2]*(b[0]*c[1]-b[1]*c[0]))/6;
      ++out.triangles;
    }
  }
  REQUIRE(index%3==0);REQUIRE(out.triangles>0);out.volume=std::abs(out.volume);REQUIRE(out.volume>0);return out;
}
std::set<std::string> dependencies(const std::filesystem::path& file,const std::filesystem::path& working)
{
  auto bytes=read(file);auto colon=bytes.find(": ");if(colon==std::string::npos) colon=bytes.find(":\n");REQUIRE(colon!=std::string::npos);
  std::set<std::string> paths;std::string token;
  auto add=[&] {if(!token.empty()){auto p=std::filesystem::u8path(token);if(p.is_relative())p=working/p;paths.insert(utf8(std::filesystem::canonical(p)));token.clear();}};
  for(size_t i=colon+1;i<bytes.size();++i) {
    char c=bytes[i];
    if(c=='\\' && i+1<bytes.size()) {char next=bytes[++i];if(next=='\r' && i+1<bytes.size() && bytes[i+1]=='\n')++i;
      if(next=='\n'||next=='\r'){add();continue;}token+=next;
    } else if(c==' '||c=='\n'||c=='\r'||c=='\t') add();else token+=c;
  }add();return paths;
}
Mesh render(const GeneratedFragment& fragment,const std::filesystem::path& directory,const std::string& name,
            const QProcessEnvironment& environment,bool hostile=false,bool subtract=false)
{
  auto source=directory/std::filesystem::u8path(name+".scad");auto stl=directory/std::filesystem::u8path(name+".stl");
  auto dep=directory/std::filesystem::u8path(name+".d");auto summary=directory/std::filesystem::u8path(name+".json");
  std::string prefix="$fn=48;\n";
  if(hostile) prefix+="BOLTS_MODE=\"bom\"; BOLTS_DEFAULT_UNIT=\"in\"; $slop=2;\n";
  std::string imports=fragment.standaloneSource.substr(0,fragment.standaloneSource.size()-fragment.geometrySource.size());
  auto sourceBytes=prefix+imports+(subtract?"difference() {\ntranslate([-10,-10,0]) cube([20,20,10]);\n"+fragment.geometrySource+"}\n":fragment.geometrySource);
  write(source,sourceBytes);
  QProcess process;process.setProcessEnvironment(environment);process.setWorkingDirectory(QString::fromStdString(utf8(directory)));
  process.start(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE),QStringList{
    "--backend","Manifold","--hardwarnings","--export-format","asciistl","--summary","all","--summary-file",QString::fromStdString(utf8(summary)),
    "-d",QString::fromStdString(utf8(dep)),"-o",QString::fromStdString(utf8(stl)),QString::fromStdString(utf8(source))});
  REQUIRE(process.waitForStarted());REQUIRE(process.waitForFinished(60000));
  auto messages=(process.readAllStandardOutput()+process.readAllStandardError()).toStdString();write(directory/std::filesystem::u8path(name+".log"),messages);
  INFO(messages);REQUIRE(process.exitStatus()==QProcess::NormalExit);REQUIRE(process.exitCode()==0);
  CHECK(messages.find("WARNING:")==std::string::npos);CHECK(messages.find("ERROR:")==std::string::npos);
  CHECK(messages.find("ECHO: \"Error")==std::string::npos);CHECK(messages.find("ECHO: \"Warning")==std::string::npos);
  std::set<std::string> expected{utf8(std::filesystem::canonical(source))};
  for(const auto& library:fragment.binding.libraries)for(const auto& file:library.files)expected.insert(file.canonicalPath);
  CHECK(dependencies(dep,directory)==expected);
  auto report=nlohmann::json::parse(read(summary));REQUIRE(report.contains("geometry"));CHECK(report["geometry"]["dimensions"]==3);
  auto result=mesh(stl);
  if(subtract){for(size_t i=0;i<3;++i){CHECK(std::abs(result.min[i]-(i==2?0:-10))<1e-6);CHECK(std::abs(result.max[i]-10)<1e-6);}CHECK(result.volume<3990);CHECK(result.volume>3000);}
  return result;
}
void definitionIdentity(const GeneratedFragment& fragment,const SymbolBindings& names,const std::filesystem::path& file)
{
  SourceFile *raw=nullptr;REQUIRE(parse(raw,fragment.standaloneSource,utf8(file),utf8(file),0));std::unique_ptr<SourceFile> parsed(raw);
  for(const auto& binding:fragment.moduleBindings) {
    const LocalScope *scope=parsed->scope.get();
    if(!fragment.helpers.empty()) scope=scope->getUserModules().at(names.begin()->second)->body.get();
    auto module=scope->getUserModules().find(binding.module);REQUIRE(module!=scope->getUserModules().end());
    auto library=std::find_if(fragment.binding.libraries.begin(),fragment.binding.libraries.end(),[&](const auto& lib){return lib.libraryId==binding.libraryId;});REQUIRE(library!=fragment.binding.libraries.end());
    CHECK(utf8(std::filesystem::canonical(module->second->location().filePath()))==utf8(std::filesystem::u8path(library->root)/std::filesystem::u8path(binding.path)));
  }
}
}
TEST_CASE("provider_shipped_catalog_matrix_matches_the_contract", "[providers-schema][providers-source]")
{
  auto snapshot=ProviderRegistry::load({{OPENSCAD_PROVIDER_TEST_DIR}});
  for(const auto& d:snapshot->diagnostics)INFO(d.path+" "+d.pointer+" "+d.message);
  REQUIRE(snapshot->diagnostics.empty());CHECK(snapshot->items.size()==8);
  for(const auto& [id,item]:snapshot->items)CHECK(item.insertionRole==(item.providerId=="scadmaxx.bolts"?InsertionRole::Component:InsertionRole::Geometry));
  CHECK_FALSE(snapshot->items.count("scadmaxx.bolts:iso4762"));
}
TEST_CASE("provider_generated_external_geometry_and_dependency_closures", "[providers-external]")
{
  const auto bosl=qEnvironmentVariable("SCADMAXX_PROVIDER_BOSL2_ROOT"),bolts=qEnvironmentVariable("SCADMAXX_PROVIDER_BOLTS_ROOT");
  if(bosl.isEmpty()||bolts.isEmpty()) SKIP("Pinned external library roots must be supplied explicitly");
  auto boslRoot=std::filesystem::canonical(std::filesystem::u8path(bosl.toStdString()));
  auto boltsRoot=std::filesystem::canonical(std::filesystem::u8path(bolts.toStdString()));
  auto directory=std::filesystem::path(OPENSCAD_PROVIDER_TEST_OUTPUT_DIR);std::filesystem::create_directories(directory);
  auto search=utf8(boslRoot.parent_path())+";"+utf8(boltsRoot.parent_path());
#ifndef _WIN32
  search=utf8(boslRoot.parent_path())+":"+utf8(boltsRoot.parent_path());
#endif
  PlatformUtils::registerApplicationPath(utf8(std::filesystem::path(OPENSCAD_PROVIDER_TEST_EXE).parent_path()));
  qputenv("OPENSCADPATH",QByteArray::fromStdString(search));parser_init();
  auto environment=QProcessEnvironment::systemEnvironment();environment.insert("OPENSCADPATH",QString::fromStdString(search));
  auto context=nativeResolutionContext(utf8(std::filesystem::canonical(directory)),{{"bosl2",{utf8(boslRoot),"BOSL2"}},{"bolts",{utf8(boltsRoot),"BOLTS"}}});REQUIRE(context);
  auto snapshot=ProviderRegistry::load({{OPENSCAD_PROVIDER_TEST_DIR}});
  for(const auto& d:snapshot->diagnostics)INFO(d.path+" "+d.pointer+" "+d.message);
  REQUIRE(snapshot->diagnostics.empty());
  for(const auto& id:std::vector<std::string>{"scadmaxx.builtin:plain-hole","scadmaxx.bosl2:threaded-hole","scadmaxx.bolts:iso4017","scadmaxx.bolts:iso4032","scadmaxx.bolts:iso7089"}) {
    auto binding=ProviderRegistry::bind(snapshot,id,*context.value);INFO(id);if(!binding)INFO(binding.diagnostic->message);REQUIRE(binding);
    SymbolBindings symbols;for(const auto& helper:snapshot->items.at(id).recipes[binding.value->recipeIndex].helpers)symbols[helper.id]="smx_test_hardware";
    for(int size:{3,4,5,6,8}) {
      INFO(size);ParameterValues values=id=="scadmaxx.builtin:plain-hole"?ParameterValues{{"diameter",double(size)}}:ParameterValues{{"size","M"+std::to_string(size)}};
      auto fragment=ProviderRegistry::generate(*binding.value,values,symbols);REQUIRE(fragment);
      auto key=snapshot->items.at(id).id+"-M"+std::to_string(size);auto result=render(*fragment.value,directory,key,environment);
      if(size==4) {
        definitionIdentity(*fragment.value,symbols,directory/std::filesystem::u8path(key+".scad"));
        Vector minimum,maximum;
        if(id.find("iso4017")!=std::string::npos){minimum={-3.5,-4.041451884327381,-2.8};maximum={3.5,4.041451884327381,20};}
        else if(id.find("iso4032")!=std::string::npos){minimum={-3.5,-4.041451884327381,0};maximum={3.5,4.041451884327381,3.2};}
        else if(id.find("iso7089")!=std::string::npos){minimum={-4.5,-4.5,0};maximum={4.5,4.5,0.8};}
        else if(id.find("threaded-hole")!=std::string::npos){minimum={-2.059218392,-2.059218392,0};maximum={2.059218392,2.059218392,12.01};}
        else {minimum={-2,-2,0};maximum={2,2,12};}
        for(size_t axis=0;axis<3;++axis){CHECK(std::abs(result.min[axis]-minimum[axis])<1e-6);CHECK(std::abs(result.max[axis]-maximum[axis])<1e-6);}
        if(!binding.value->libraries.empty()){
          auto hostile=render(*fragment.value,directory,key+"-ambient",environment,true);
          for(size_t axis=0;axis<3;++axis){CHECK(std::abs(hostile.min[axis]-result.min[axis])<1e-6);CHECK(std::abs(hostile.max[axis]-result.max[axis])<1e-6);}CHECK(std::abs(hostile.volume-result.volume)<1e-6);
        }
        if(fragment.value->operation==Operation::Subtractive)render(*fragment.value,directory,key+"-cavity",environment,false,true);
      }
    }
  }
}
