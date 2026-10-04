// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/LibraryResolution.h"
#include "core/providers/ContentDigest.h"
#include "core/providers/CatalogLoader.h"
#include "core/parsersettings.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Providers {
namespace {
std::string utf8(const std::filesystem::path& p) { auto s=p.generic_u8string(); return {s.begin(),s.end()}; }
}
std::string resolutionContextDigest(const LibraryResolutionContext& context)
{
  nlohmann::json j{{"sourceDirectory",context.sourceDirectory},{"revision",context.revision},{"searchPaths",context.searchPaths}};
  j["bindings"]=nlohmann::json::array();
  for (const auto& [id,location] : context.libraryBindings)
    j["bindings"].push_back({id,location.root,location.importPrefix});
  return sha256(j.dump());
}
Result<LibraryResolutionContext> nativeResolutionContext(const std::string& sourceDirectory,
  const std::map<std::string,LibraryLocation>& bindings)
{
  try {
    LibraryResolutionContext context;
    context.sourceDirectory=utf8(std::filesystem::canonical(std::filesystem::u8path(sourceDirectory)));
    if (!std::filesystem::is_directory(std::filesystem::u8path(context.sourceDirectory)))
      throw std::runtime_error("Source directory is not a directory");
    context.searchPaths=get_library_path();
    for (const auto& [id,location] : bindings) {
      if (!validRelativePath(location.importPrefix,true)) throw std::runtime_error("Invalid import prefix: " + id);
      context.libraryBindings.emplace(id,LibraryLocation{
        utf8(std::filesystem::weakly_canonical(std::filesystem::u8path(location.root))),location.importPrefix});
    }
    const auto capturedPaths=context.searchPaths;
    context.resolveImport=[capturedPaths](const std::string& from,const std::string& spelling)->Result<std::string> {
      if (capturedPaths!=get_library_path()) return {{},Diagnostic{"StaleBinding",from,"","Native search paths changed"}};
      try {
        auto path=find_valid_path(std::filesystem::u8path(from),std::filesystem::u8path(spelling));
        if (path.empty()) return {{},Diagnostic{"MissingLibrary",spelling,"","Native import was not found"}};
        return {utf8(std::filesystem::canonical(path)),{}};
      } catch (const std::exception& e) { return {{},Diagnostic{"ResolutionMismatch",spelling,"",e.what()}}; }
    };
    context.readBytes=[](const std::string& path)->Result<SourceBytes> {
      try {
        auto native=std::filesystem::u8path(path);
        if (std::filesystem::file_size(native)>16*1024*1024) return {{},Diagnostic{"LimitExceeded",path,"","Library file exceeds 16 MiB"}};
        std::ifstream in(native,std::ios::binary); if (!in) throw std::runtime_error("Cannot read library file");
        std::ostringstream bytes; bytes<<in.rdbuf(); if (in.bad()) throw std::runtime_error("Library read failed");
        auto data=bytes.str(); auto revision=sha256(data);
        return {SourceBytes{std::move(data),std::move(revision)},{}};
      } catch (const std::exception& e) { return {{},Diagnostic{"MissingLibrary",path,"",e.what()}}; }
    };
    context.revision=resolutionContextDigest(context);
    return {std::move(context),{}};
  } catch (const std::exception& e) { return {{},Diagnostic{"ResolutionMismatch",sourceDirectory,"",e.what()}}; }
}
}
