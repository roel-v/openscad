// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/ProviderRegistry.h"
#include "core/providers/CatalogLoader.h"
#include "core/providers/ScadFragmentWriter.h"
#include "core/providers/LibraryMatcher.h"
#include "core/providers/LibraryResolution.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace Providers {
namespace {
std::string utf8(const std::filesystem::path& p) { auto s=p.generic_u8string(); return {s.begin(),s.end()}; }
}
std::shared_ptr<const RegistrySnapshot> ProviderRegistry::load(const CatalogSources& sources)
{
  static std::atomic<uint64_t> generation{0};
  auto out=std::make_shared<RegistrySnapshot>(); out->generation=++generation;
  std::set<std::string> paths;
  for (const auto& source:sources.paths) {
    try {
      auto p=std::filesystem::u8path(source);
      if (!std::filesystem::exists(p)) {
        if (p.extension()==".json") out->diagnostics.push_back({"InvalidCatalog",source,"","Catalog file is missing"});
        continue;
      }
      if (std::filesystem::is_directory(p)) {
        for (const auto& entry:std::filesystem::directory_iterator(p))
          if (entry.is_regular_file() && entry.path().extension()==".json") paths.insert(utf8(std::filesystem::canonical(entry.path())));
      } else paths.insert(utf8(std::filesystem::canonical(p)));
    } catch(const std::exception& e) { out->diagnostics.push_back({"InvalidCatalog",source,"",e.what()}); }
  }
  for (const auto& path:paths) {
    try {
      auto p=std::filesystem::u8path(path);
      if (std::filesystem::file_size(p)>4*1024*1024) { out->diagnostics.push_back({"LimitExceeded",path,"","Catalog exceeds 4 MiB"}); continue; }
      std::ifstream file(p,std::ios::binary); if (!file) throw std::runtime_error("Cannot read catalog");
      std::ostringstream bytes; bytes<<file.rdbuf(); if (file.bad()) throw std::runtime_error("Catalog read failed");
      auto result=loadCatalog(bytes.str(),path);
      if (!result) { out->diagnostics.push_back(*result.diagnostic); continue; }
      auto provider=std::move(*result.value);
      for (const auto& prior:out->providers) if (prior.id==provider.id) {
        Diagnostic d{"DuplicateCatalog",provider.path,"/providerId","Provider " + provider.id + " also declared by " + prior.path};
        out->disabledProviders[provider.id]=d; out->diagnostics.push_back(d);
        out->diagnostics.push_back({"DuplicateCatalog",prior.path,"/providerId","Provider " + provider.id + " also declared by " + provider.path});
      }
      for (const auto& item:provider.items) out->items.emplace(item.qualifiedId,item);
      out->providers.push_back(std::move(provider));
    } catch(const std::exception& e) { out->diagnostics.push_back({"InvalidCatalog",path,"",e.what()}); }
  }
  return out;
}
Result<GeneratedFragment> ProviderRegistry::generate(const ProviderBinding& binding,const ParameterValues& values,const SymbolBindings& names)
{ return generateFragment(binding,values,names); }
Result<ProviderBinding> ProviderRegistry::bind(std::shared_ptr<const RegistrySnapshot> snapshot,
  const std::string& id,const LibraryResolutionContext& context)
{
  auto fail=[&](const std::string& code,const std::string& message)->Result<ProviderBinding> {
    return {{},Diagnostic{code,id,"",message}};
  };
  if (!snapshot || !snapshot->items.count(id)) return fail("UnknownItem","Unknown qualified item ID");
  const auto& item=snapshot->items.at(id);
  auto duplicate=snapshot->disabledProviders.find(item.providerId);
  if (duplicate!=snapshot->disabledProviders.end()) return {{},duplicate->second};
  std::vector<ProviderBinding> matches;
  std::optional<Diagnostic> firstFailure;
  auto priority=[](const Diagnostic& d){
    if(d.code=="StaleBinding") return 0; if(d.code=="ResolutionMismatch") return 1;
    if(d.code=="ModifiedLibrary") return 2; if(d.code=="UnsupportedRevision") return 3;
    if(d.code=="MissingLibrary") return 4; return 5;
  };
  for (size_t index=0;index<item.recipes.size();++index) {
    const auto& recipe=item.recipes[index];
    ProviderBinding binding{snapshot,id,item.catalogDigest,context.revision,resolutionContextDigest(context),index,{}};
    bool matched=true;
    for (const auto& profileId:recipe.profileIds) {
      const LibraryRequirement *library=nullptr; const LibraryProfile *profile=nullptr;
      for (const auto& candidate:item.libraries) for (const auto& p:candidate.profiles)
        if(p.id==profileId) {library=&candidate;profile=&p;}
      if(!library || !profile) return fail("InvalidCatalog","Missing owned profile");
      auto result=matchLibrary(*library,*profile,recipe,context);
      if(!result) {
        matched=false;
        if(!firstFailure || priority(*result.diagnostic)<priority(*firstFailure)) firstFailure=result.diagnostic;
      } else binding.libraries.push_back(std::move(*result.value));
    }
    if(matched) matches.push_back(std::move(binding));
  }
  if(matches.size()>1) return fail("AmbiguousProfile","More than one recipe matches the effective library closure");
  if(matches.empty()) {
    if(firstFailure) return {{},firstFailure};
    return fail("InvalidCatalog","Item has no recipe");
  }
  return {std::move(matches.front()),{}};
}
std::vector<ItemStatus> ProviderRegistry::query(std::shared_ptr<const RegistrySnapshot> snapshot,
  const LibraryResolutionContext& context)
{
  std::vector<ItemStatus> statuses;
  if(!snapshot) return statuses;
  for(const auto& [id,item]:snapshot->items) {
    auto result=bind(snapshot,id,context);
    statuses.push_back({id,item.label,result?Availability::Available:diagnosticAvailability(*result.diagnostic),result.diagnostic,std::move(result.value)});
  }
  return statuses;
}
Result<ProviderBinding> ProviderRegistry::revalidate(const ProviderBinding& binding,
  const LibraryResolutionContext& context,std::shared_ptr<const RegistrySnapshot> currentSnapshot)
{
  auto stale=[&](const std::string& message)->Result<ProviderBinding> {
    return {{},Diagnostic{"StaleBinding",binding.qualifiedId,"",message}};
  };
  if(!binding.snapshot || !currentSnapshot || currentSnapshot->generation!=binding.snapshot->generation ||
     !currentSnapshot->items.count(binding.qualifiedId) || currentSnapshot->items.at(binding.qualifiedId).catalogDigest!=binding.catalogDigest)
    return stale("Registry generation/catalog changed");
  if(context.revision!=binding.contextRevision || resolutionContextDigest(context)!=binding.contextDigest)
    return stale("Source context or ordered library paths changed");
  auto fresh=bind(std::move(currentSnapshot),binding.qualifiedId,context);
  if(!fresh) {
    auto diagnostic=*fresh.diagnostic; diagnostic.message=diagnostic.code+": "+diagnostic.message;
    diagnostic.code="StaleBinding"; return {{},std::move(diagnostic)};
  }
  if(fresh.value->recipeIndex!=binding.recipeIndex || fresh.value->libraries.size()!=binding.libraries.size())
    return stale("Selected recipe changed");
  for(size_t i=0;i<binding.libraries.size();++i) {
    const auto& old=binding.libraries[i]; const auto& now=fresh.value->libraries[i];
    if(old.root!=now.root || old.profileId!=now.profileId || old.importPrefix!=now.importPrefix || old.files.size()!=now.files.size())
      return stale("Resolved library changed");
    for(size_t j=0;j<old.files.size();++j) {
      const auto& a=old.files[j]; const auto& b=now.files[j];
      if(a.canonicalPath!=b.canonicalPath || a.relativePath!=b.relativePath || a.normalizedHash!=b.normalizedHash || a.rawRevision!=b.rawRevision)
        return stale("Captured library content/revision changed: "+a.relativePath);
    }
  }
  return fresh;
}
const char *roleName(InsertionRole r) { return r==InsertionRole::Geometry ? "geometry" : "component"; }
const char *availabilityName(Availability a)
{
  switch(a) {
    case Availability::Available:return "Available"; case Availability::MissingLibrary:return "MissingLibrary";
    case Availability::UnsupportedRevision:return "UnsupportedRevision"; case Availability::ModifiedLibrary:return "ModifiedLibrary";
    case Availability::ResolutionMismatch:return "ResolutionMismatch"; case Availability::InvalidCatalog:return "InvalidCatalog";
    case Availability::DuplicateCatalog:return "DuplicateCatalog"; case Availability::AmbiguousProfile:return "AmbiguousProfile";
    case Availability::StaleBinding:return "StaleBinding";
  }
  return "InvalidCatalog";
}
}
