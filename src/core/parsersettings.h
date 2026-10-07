#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern int parser_error_pos;

/**
 * Initialize library path.
 */
void parser_init();

fs::path search_libs(const fs::path& localpath);
fs::path find_valid_path(const fs::path& sourcepath, const fs::path& localpath,
                         const std::vector<std::string> *openfilenames = nullptr);
fs::path get_library_for_path(const fs::path& localpath);

const std::vector<std::string>& get_library_path();
// Caller owns the native execution gate; replaces the complete ordered search path.
void set_library_path(std::vector<std::string> orderedPaths);

// Record actual include/use searches, including their recursion exclusions. A
// cache must re-run these searches to detect newly appearing shadowing files.
struct LibraryPathLookup {
  fs::path source, local, resolved;
  std::vector<std::string> openFiles;
  bool operator==(const LibraryPathLookup& other) const {
    return source == other.source && local == other.local && resolved == other.resolved && openFiles == other.openFiles;
  }
};
class LibraryPathLookupScope {
public:
  explicit LibraryPathLookupScope(std::vector<LibraryPathLookup>& output);
  ~LibraryPathLookupScope();
  LibraryPathLookupScope(const LibraryPathLookupScope&) = delete;
  LibraryPathLookupScope& operator=(const LibraryPathLookupScope&) = delete;
  static bool current(const std::vector<LibraryPathLookup>& lookups);
private:
  std::vector<LibraryPathLookup> *previous;
};
