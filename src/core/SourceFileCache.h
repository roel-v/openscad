#pragma once

#include <ctime>
#include <string>
#include <unordered_map>

class SourceFile;
class EvaluationInputs;

/*!
   Caches parsed SourceFiles based on their filenames and mtimes.
 */
class SourceFileCache
{
public:
  static SourceFileCache *instance()
  {
    if (active) return active;
    if (!inst) inst = new SourceFileCache;
    return inst;
  }

  std::time_t process(const std::string& mainFile, const std::string& filename,
                      SourceFile *& sourceFile);
  SourceFile *lookup(const std::string& filename);
  // Seed a request-local cache from an already captured AST; never reparses current disk.
  void seedCaptured(const std::string& fileKey, const SourceFile&);
  // Inputs must already contain the independently validated prior parse closure.
  void seedCapturedLibraries(const SourceFileCache& previous);
  size_t size() const { return this->entries.size(); }
  void clear();
  static void clear_markers();
  explicit SourceFileCache(EvaluationInputs *inputs = nullptr) : inputs(inputs) {}
  ~SourceFileCache();
  SourceFileCache(const SourceFileCache&) = delete;
  SourceFileCache& operator=(const SourceFileCache&) = delete;
  static SourceFileCache *setActive(SourceFileCache *cache);

private:
  static SourceFileCache *inst;
  static thread_local SourceFileCache *active;
  EvaluationInputs *inputs;

  struct cache_entry {
    SourceFile *file{};  // This is only valid if parsing succeeded?
    SourceFile
      *parsed_file{};  // the last version parsed for the include list, can exist even if parsing failed
    std::string cache_id;
    std::time_t mtime{};           // time file last modified
    std::time_t includes_mtime{};  // time the includes last changed
  };
  std::unordered_map<std::string, cache_entry> entries;
  // Request-local only: avoid filesystem identity work on every function/module lookup.
  // unordered_map keeps element addresses stable across insertions/rehashes.
  // Cache the entry, not its file: processing may populate a previously failed parse.
  std::unordered_map<std::string, cache_entry *> resolvedEntries;
};
