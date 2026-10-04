// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/LibraryMatcher.h"
#include "core/providers/CatalogLoader.h"
#include "core/providers/ContentDigest.h"
#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>

namespace Providers {
namespace {
std::string utf8(const std::filesystem::path& p) { auto s=p.generic_u8string(); return {s.begin(),s.end()}; }
std::string joined(const std::string& root,const std::string& relative)
{ return utf8((std::filesystem::u8path(root)/std::filesystem::u8path(relative)).lexically_normal()); }
bool absoluteCanonical(const std::string& path)
{
  auto p=std::filesystem::u8path(path);
  return p.is_absolute() && utf8(p.lexically_normal())==path;
}
std::string marker(const std::map<std::string,std::string>& bytes)
{
  std::smatch match;
  auto version=bytes.find("version.scad");
  if (version!=bytes.end() && std::regex_search(version->second,match,
      std::regex("BOSL_VERSION\\s*=\\s*\\[\\s*([0-9]+)\\s*,\\s*([0-9]+)\\s*,\\s*([0-9]+)\\s*\\]")))
    return match[1].str()+"."+match[2].str()+"."+match[3].str();
  auto bolts=bytes.find("common/version.scad");
  if (bolts!=bytes.end() && std::regex_search(bolts->second,match,
      std::regex("function\\s+BOLTS_version\\s*\\(\\s*\\)\\s*=\\s*\"([^\"]*)\"")))
    return match[1].str();
  return "unreported";
}
}
Availability diagnosticAvailability(const Diagnostic& d)
{
  if(d.code=="MissingLibrary") return Availability::MissingLibrary;
  if(d.code=="UnsupportedRevision") return Availability::UnsupportedRevision;
  if(d.code=="ModifiedLibrary") return Availability::ModifiedLibrary;
  if(d.code=="ResolutionMismatch") return Availability::ResolutionMismatch;
  if(d.code=="DuplicateCatalog") return Availability::DuplicateCatalog;
  if(d.code=="AmbiguousProfile") return Availability::AmbiguousProfile;
  if(d.code=="StaleBinding") return Availability::StaleBinding;
  return Availability::InvalidCatalog;
}
Result<ResolvedLibrary> matchLibrary(const LibraryRequirement& library,const LibraryProfile& profile,
  const Recipe& recipe,const LibraryResolutionContext& context)
{
  Diagnostic evidence; evidence.libraryId=library.id;
  for (const auto& p:library.profiles) evidence.expectedProfiles.push_back(p.id+" ("+p.releaseLabel+")");
  auto fail=[&](const std::string& code,const std::string& path,const std::string& message)->Result<ResolvedLibrary> {
    evidence.code=code; evidence.path=path; evidence.message=message; return {{},evidence};
  };
  if (!context.resolveImport || !context.readBytes || !absoluteCanonical(context.sourceDirectory))
    return fail("ResolutionMismatch",context.sourceDirectory,"Context needs an absolute source directory and both captured-input callbacks");
  std::string prefix=library.defaultImportPrefix,root;
  auto configured=context.libraryBindings.find(library.id);
  if (configured!=context.libraryBindings.end()) { prefix=configured->second.importPrefix; root=configured->second.root; }
  if (!validRelativePath(prefix,true) || (!root.empty() && !absoluteCanonical(root)))
    return fail("ResolutionMismatch",root,"Invalid canonical root/import prefix");
  try {
    std::set<std::string> imports;
    for (const auto& imp:recipe.imports) if (imp.libraryId==library.id) imports.insert(imp.path);
    if (imports.empty()) return fail("InvalidCatalog",library.id,"Selected library has no root import");
    for (const auto& path:imports) {
      auto resolved=context.resolveImport(context.sourceDirectory,prefix.empty()?path:prefix+"/"+path);
      if (!resolved) return fail(resolved.diagnostic->code,resolved.diagnostic->path,resolved.diagnostic->message);
      if (!absoluteCanonical(*resolved.value)) return fail("ResolutionMismatch",*resolved.value,"Resolver returned a noncanonical path");
      if (root.empty()) {
        auto candidate=std::filesystem::u8path(*resolved.value);
        for (const auto& component:std::filesystem::u8path(path)) candidate=candidate.parent_path();
        root=utf8(candidate);
      }
      evidence.effectiveRoot=root;
      if (*resolved.value!=joined(root,path)) return fail("ResolutionMismatch",*resolved.value,"Effective root import differs from the selected library root");
    }
    ResolvedLibrary out{library.id,profile.id,profile.releaseLabel,"",root,prefix,profile.digest,{}};
    std::map<std::string,std::string> content;
    size_t total=0;
    for (const auto& [relative,expectedHash]:profile.fileHashes) {
      auto path=joined(root,relative);
      auto resolved=context.resolveImport(utf8(std::filesystem::u8path(path).parent_path()),utf8(std::filesystem::u8path(path).filename()));
      if (!resolved || *resolved.value!=path) {
        evidence.firstMismatchingFile=relative;
        return fail("ResolutionMismatch",path,"Profile file is missing, shadowed or resolves through an escaping symlink");
      }
      auto bytes=context.readBytes(path);
      if (!bytes) { evidence.firstMismatchingFile=relative; return fail(bytes.diagnostic->code,path,bytes.diagnostic->message); }
      total+=bytes.value->bytes.size();
      if (bytes.value->bytes.size()>16*1024*1024 || total>128*1024*1024)
        return fail("LimitExceeded",path,"Library capture exceeds resource limit");
      auto normalized=normalizedUtf8(bytes.value->bytes,path);
      if (!normalized) { evidence.firstMismatchingFile=relative; return fail("ModifiedLibrary",path,normalized.diagnostic->message); }
      auto hash=sha256(*normalized.value); evidence.detectedHashes[relative]=hash;
      if (hash!=expectedHash && evidence.firstMismatchingFile.empty()) evidence.firstMismatchingFile=relative;
      content.emplace(relative,std::move(*normalized.value));
      out.files.push_back({relative,path,hash,bytes.value->revision});
    }
    evidence.detectedMarker=marker(content); out.detectedMarker=evidence.detectedMarker;
    if (!evidence.firstMismatchingFile.empty()) {
      bool knownMarker=evidence.detectedMarker=="unreported" || evidence.detectedMarker=="development";
      for (const auto& p:library.profiles) knownMarker|=p.releaseLabel==evidence.detectedMarker;
      return fail(knownMarker?"ModifiedLibrary":"UnsupportedRevision",joined(root,evidence.firstMismatchingFile),"Content differs from the exact supported profile");
    }
    for (const auto& edge:profile.importEdges) {
      auto from=utf8(std::filesystem::u8path(joined(root,edge.from)).parent_path());
      auto target=context.resolveImport(from,edge.spelling);
      if (!target || *target.value!=joined(root,edge.to)) {
        evidence.firstMismatchingFile=edge.from;
        return fail("ResolutionMismatch",edge.from,"Frozen import edge "+edge.spelling+" resolves outside the expected closure");
      }
    }
    // Detect a changing reader or installation during capture, including changes that preserve timestamps/size.
    for (const auto& file:out.files) {
      auto bytes=context.readBytes(file.canonicalPath);
      if (!bytes) return fail("StaleBinding",file.canonicalPath,"Captured input became unavailable");
      auto normalized=normalizedUtf8(bytes.value->bytes,file.canonicalPath);
      if (!normalized || bytes.value->revision!=file.rawRevision || sha256(*normalized.value)!=file.normalizedHash)
        return fail("StaleBinding",file.canonicalPath,"Library bytes changed during capture");
    }
    return {std::move(out),{}};
  } catch(const std::exception& e) { return fail("ResolutionMismatch",root,e.what()); }
}
}
