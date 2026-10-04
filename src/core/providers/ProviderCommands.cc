// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/ProviderCommands.h"
#include "core/providers/CatalogLoader.h"
#include "core/providers/LibraryResolution.h"
#include "core/providers/ProviderRegistry.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>

namespace Providers {
namespace {
using Json=nlohmann::json;
std::string utf8(const std::filesystem::path& p) {auto s=p.generic_u8string();return {s.begin(),s.end()};}
std::string absolute(const std::string& directory,const std::string& value)
{
  auto path=std::filesystem::u8path(value);
  if(path.is_relative())path=std::filesystem::u8path(directory)/path;
  return utf8(std::filesystem::absolute(path).lexically_normal());
}
Json diagnosticJson(const Diagnostic& d)
{ return {{"code",d.code},{"path",d.path},{"pointer",d.pointer},{"message",d.message},{"libraryId",d.libraryId},
  {"effectiveRoot",d.effectiveRoot},{"expectedProfiles",d.expectedProfiles},{"detectedMarker",d.detectedMarker},
  {"firstMismatchingFile",d.firstMismatchingFile},{"detectedHashes",d.detectedHashes}}; }
Result<Json> readJson(const std::string& path)
{
  try {
    auto file=std::filesystem::u8path(path);
    if(std::filesystem::file_size(file)>4*1024*1024)return {{},Diagnostic{"LimitExceeded",path,"","JSON file exceeds 4 MiB"}};
    std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("Cannot read JSON file");
    std::ostringstream bytes;bytes<<input.rdbuf();if(input.bad())throw std::runtime_error("JSON read failed");
    return parseJson(bytes.str(),path);
  } catch(const std::exception& e){return {{},Diagnostic{"InvalidJson",path,"",e.what()}};}
}
Json scalar(const ScalarValue& value) {return std::visit([](const auto& v)->Json{return v;},value);}
Json expression(const Expression& e)
{
  if(e.kind==Expression::Kind::Constant)return {{"constant",scalar(e.constant)}};
  if(e.kind==Expression::Kind::Parameter)return {{"parameter",e.name}};
  auto arguments=Json::array();for(const auto& arg:e.arguments)arguments.push_back(expression(*arg));
  if(e.kind==Expression::Kind::Vector)return {{"vector",arguments}};
  return {{"operator",e.name},{"arguments",arguments}};
}
Json statement(const Statement& s)
{
  auto arguments=Json::array(),children=Json::array();
  for(const auto& [name,value]:s.arguments)arguments.push_back({{"name",name},{"value",expression(*value)}});
  for(const auto& child:s.children)children.push_back(statement(child));
  return {{"module",s.helper?Json{{"helper",s.module}}:Json(s.module)},{"arguments",arguments},{"children",children}};
}
Json importJson(const ImportRequest& i)
{return {{"libraryId",i.libraryId},{"path",i.path},{"kind",i.kind==ImportRequest::Kind::Include?"include":"use"},
  {"scope",i.scope==ImportRequest::Scope::File?"file":"helper"},{"helperId",i.helperId}};}
Json bindingJson(const ProviderBinding& binding)
{
  auto libraries=Json::array();
  for(const auto& lib:binding.libraries) {
    auto files=Json::array();for(const auto& file:lib.files)files.push_back({{"relativePath",file.relativePath},
      {"canonicalPath",file.canonicalPath},{"normalizedHash",file.normalizedHash},{"rawRevision",file.rawRevision}});
    libraries.push_back({{"libraryId",lib.libraryId},{"profileId",lib.profileId},{"releaseLabel",lib.releaseLabel},
      {"detectedMarker",lib.detectedMarker},{"root",lib.root},{"importPrefix",lib.importPrefix},{"digest",lib.digest},{"files",files}});
  }
  return {{"qualifiedId",binding.qualifiedId},{"generation",binding.snapshot->generation},{"catalogDigest",binding.catalogDigest},
    {"contextRevision",binding.contextRevision},{"contextDigest",binding.contextDigest},{"recipeIndex",binding.recipeIndex},{"libraries",libraries}};
}
Json fragmentJson(const GeneratedFragment& f)
{
  auto values=Json::object(),imports=Json::array(),helpers=Json::array(),modules=Json::array();
  for(const auto& [name,value]:f.parameters)values[name]=scalar(value);
  for(const auto& i:f.fileImports)imports.push_back(importJson(i));
  for(const auto& h:f.helpers){auto local=Json::array();for(const auto& i:h.localImports)local.push_back(importJson(i));
    helpers.push_back({{"id",h.id},{"parameters",h.parameters},{"body",statement(h.body)},{"localImports",local}});}
  for(const auto& b:f.moduleBindings)modules.push_back({{"module",b.module},{"libraryId",b.libraryId},{"path",b.path}});
  return {{"binding",bindingJson(f.binding)},{"parameters",values},{"fileImports",imports},{"helpers",helpers},
    {"geometry",statement(f.geometry)},{"moduleBindings",modules},{"operation",f.operation==Operation::Additive?"additive":"subtractive"},
    {"insertionRole",roleName(f.insertionRole)},{"frame",{{"origin",f.frame.origin},{"normal",f.frame.normal},{"up",f.frame.up},{"label",f.frame.label}}},
    {"helperSource",f.helperSource},{"geometrySource",f.geometrySource},{"provenanceComment",f.provenanceComment},{"standaloneSource",f.standaloneSource}};
}
Result<ParameterValues> values(const std::string& path)
{
  if(path.empty())return {ParameterValues{}, {}};
  auto json=readJson(path);if(!json)return {{},json.diagnostic};
  if(!json.value->is_object())return {{},Diagnostic{"InvalidParameter",path,"","Values must be a JSON object"}};
  ParameterValues result;
  for(const auto& [key,value]:json.value->items()) {
    if(value.is_boolean())result[key]=value.get<bool>();
    else if(value.is_string())result[key]=value.get<std::string>();
    else if(value.is_number_integer() && (!value.is_number_unsigned()||value.get<uint64_t>()<=uint64_t(INT64_MAX))) result[key]=value.get<int64_t>();
    else if(value.is_number_float()) result[key]=value.get<double>();
    else return {{},Diagnostic{"InvalidParameter",path,"/"+key,"Expected a finite scalar within int64 range"}};
  }
  return {std::move(result),{}};
}
}
DiscoveredProviders discoverProviders(const ProviderDirectories& directories)
{
  DiscoveredProviders result;
  if(!directories.resources.empty())result.catalogs.paths.push_back(directories.resources);
  if(!directories.userConfig.empty())result.catalogs.paths.push_back(absolute(directories.userConfig,"providers"));
  if(!directories.sourceDirectory.empty())result.catalogs.paths.push_back(absolute(directories.sourceDirectory,".scadmaxx/providers"));
  result.catalogs.paths.insert(result.catalogs.paths.end(),directories.extraCatalogs.begin(),directories.extraCatalogs.end());
  if(directories.userConfig.empty())return result;
  auto configPath=absolute(directories.userConfig,"provider-sources.json");
  std::error_code inspectionError;
  auto configExists=std::filesystem::exists(std::filesystem::u8path(configPath),inspectionError);
  if(inspectionError){result.diagnostics.push_back({"InvalidSources",configPath,"",inspectionError.message()});return result;}
  if(!configExists)return result;
  auto parsed=readJson(configPath);
  if(!parsed){result.diagnostics.push_back(*parsed.diagnostic);return result;}
  auto invalid=[&](const std::string& pointer,const std::string& message){result.diagnostics.push_back({"InvalidSources",configPath,pointer,message});};
  const auto& config=*parsed.value;
  if(!config.is_object()){invalid("","Expected configuration object");return result;}
  for(const auto& [key,value]:config.items())if(key!="schemaVersion"&&key!="catalogPaths"&&key!="libraryBindings"){invalid("/"+key,"Unknown field");return result;}
  if(!config.contains("schemaVersion")||!config["schemaVersion"].is_number_integer()||config["schemaVersion"]!=1){invalid("/schemaVersion","Expected schemaVersion 1");return result;}
  if(!config.contains("catalogPaths")||!config["catalogPaths"].is_array()||config["catalogPaths"].size()>1024||
     !config.contains("libraryBindings")||!config["libraryBindings"].is_array()||config["libraryBindings"].size()>128){invalid("","Expected bounded catalogPaths/libraryBindings arrays");return result;}
  std::vector<std::string> catalogs;std::map<std::string,LibraryLocation> bindings;
  try {
    for(size_t index=0;index<config["catalogPaths"].size();++index) {
      const auto& value=config["catalogPaths"][index];
      if(!value.is_string()||value.get<std::string>().empty()){invalid("/catalogPaths/"+std::to_string(index),"Expected nonempty path");return result;}
      catalogs.push_back(absolute(directories.userConfig,value.get<std::string>()));
    }
    for(size_t index=0;index<config["libraryBindings"].size();++index) {
      auto p="/libraryBindings/"+std::to_string(index);const auto& b=config["libraryBindings"][index];
      if(!b.is_object()||b.size()!=3||!b.contains("libraryId")||!b.contains("root")||!b.contains("importPrefix")||
         !b["libraryId"].is_string()||!b["root"].is_string()||!b["importPrefix"].is_string()){invalid(p,"Expected libraryId/root/importPrefix strings");return result;}
      auto id=b["libraryId"].get<std::string>(),prefix=b["importPrefix"].get<std::string>(),root=b["root"].get<std::string>();
      if(root.empty()||!std::regex_match(id,std::regex("[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)*"))||!validRelativePath(prefix,true)||
         !bindings.emplace(id,LibraryLocation{absolute(directories.userConfig,root),prefix}).second){invalid(p,"Invalid/duplicate library binding");return result;}
    }
  }catch(const std::exception& e){invalid("",e.what());return result;}
  result.catalogs.paths.insert(result.catalogs.paths.end(),catalogs.begin(),catalogs.end());result.libraryBindings=std::move(bindings);return result;
}
int providerUsageError(const std::string& message,std::ostream& output)
{
  output<<Json{{"schemaVersion",1},{"diagnostics",Json::array({diagnosticJson({"UsageError","","",message})})}}.dump()<<"\n";return 2;
}
int runProviderCommand(const ProviderCommand& command,const ProviderCommandEnvironment& environment,std::ostream& output)
{
  Json report{{"schemaVersion",1},{"diagnostics",Json::array()}};
  auto fail=[&](const Diagnostic& d){report["diagnostics"].push_back(diagnosticJson(d));output<<report.dump()<<"\n";return 1;};
  try {
    auto source=command.sourcePath.empty()?absolute(environment.workingDirectory,"."):absolute(environment.workingDirectory,command.sourcePath);
    auto directory=source;
    if(!command.sourcePath.empty()&&!std::filesystem::is_directory(std::filesystem::u8path(source)))directory=utf8(std::filesystem::u8path(source).parent_path());
    report["sourceDirectory"]=directory;report["sourceContext"]=command.sourcePath.empty()?"cwd":"provider-source";
    auto extra=command.extraCatalogs;for(auto& path:extra)path=absolute(environment.workingDirectory,path);
    auto discovered=discoverProviders({environment.resources,environment.userConfig,directory,extra});
    std::string checked;
    if(command.mode==ProviderCommand::Mode::Check){
      checked=absolute(environment.workingDirectory,command.checkCatalog);
      std::error_code error;auto canonical=std::filesystem::canonical(std::filesystem::u8path(checked),error);
      if(!error)checked=utf8(canonical);
      discovered.catalogs.paths.push_back(checked);
    }
    for(const auto& d:discovered.diagnostics)report["diagnostics"].push_back(diagnosticJson(d));
    auto snapshot=ProviderRegistry::load(discovered.catalogs);report["generation"]=snapshot->generation;
    for(const auto& d:snapshot->diagnostics)report["diagnostics"].push_back(diagnosticJson(d));
    auto context=nativeResolutionContext(directory,discovered.libraryBindings);if(!context)return fail(*context.diagnostic);
    report["sourceDirectory"]=context.value->sourceDirectory;
    if(command.mode==ProviderCommand::Mode::Emit) {
      auto binding=ProviderRegistry::bind(snapshot,command.qualifiedId,*context.value);if(!binding)return fail(*binding.diagnostic);
      auto parameters=values(command.valuesPath.empty()?"":absolute(environment.workingDirectory,command.valuesPath));if(!parameters)return fail(*parameters.diagnostic);
      SymbolBindings symbols;size_t index=0;for(const auto& h:snapshot->items.at(command.qualifiedId).recipes[binding.value->recipeIndex].helpers)symbols[h.id]="smx_provider_helper_"+std::to_string(++index);
      auto fragment=ProviderRegistry::generate(*binding.value,*parameters.value,symbols);if(!fragment)return fail(*fragment.diagnostic);
      report["fragment"]=fragmentJson(*fragment.value);output<<report.dump()<<"\n";return 0;
    }
    report["items"]=Json::array();bool checkedPresent=false,checkedValid=true;
    for(const auto& provider:snapshot->providers)if(provider.path==checked){checkedPresent=true;if(snapshot->disabledProviders.count(provider.id))checkedValid=false;}
    for(const auto& status:ProviderRegistry::query(snapshot,*context.value)) {
      const auto& item=snapshot->items.at(status.qualifiedId);
      if(command.mode==ProviderCommand::Mode::Check&&item.catalogPath!=checked)continue;
      Json row{{"qualifiedId",status.qualifiedId},{"label",status.label},{"category",item.category},{"description",item.description},
        {"availability",availabilityName(status.availability)},{"operation",item.operation==Operation::Additive?"additive":"subtractive"},{"insertionRole",roleName(item.insertionRole)}};
      row["parameters"]=Json::array();for(const auto& p:item.parameters)row["parameters"].push_back({{"id",p.id},{"label",p.label},{"default",scalar(p.defaultValue)},{"unit",p.unit},
        {"type",p.type==ParameterSpec::Type::Number?"number":p.type==ParameterSpec::Type::Integer?"integer":p.type==ParameterSpec::Type::Bool?"bool":p.type==ParameterSpec::Type::Enum?"enum":"string"},
        {"min",p.min?Json(*p.min):Json(nullptr)},{"max",p.max?Json(*p.max):Json(nullptr)},{"exclusiveMin",p.exclusiveMin},{"enumValues",p.enumValues},{"maxLength",p.maxLength}});
      if(status.binding)row["binding"]=bindingJson(*status.binding);
      if(status.diagnostic){row["diagnostic"]=diagnosticJson(*status.diagnostic);checkedValid=false;}
      report["items"].push_back(std::move(row));
    }
    output<<report.dump()<<"\n";
    return command.mode==ProviderCommand::Mode::List?0:checkedPresent&&checkedValid?0:1;
  }catch(const std::exception& e){return fail({"ProviderError","","",e.what()});}
}
}
