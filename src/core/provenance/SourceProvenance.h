#pragma once

#include "geometry/linalg.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ASTNode;
class Assignment;
class EvaluationInputs;
class Location;
class CSGNode;
struct SourceInput;

namespace SourceProvenance {
// Every range refers to exact captured UTF-8, with an exclusive end.
struct Span {
  std::string fileKey, fingerprint;
  uint64_t begin = 0, end = 0;
};
struct SyntaxKey {
  std::string fileKey;
  uint64_t ordinal = 0;
  bool operator==(const SyntaxKey& other) const {
    return fileKey == other.fileKey && ordinal == other.ordinal;
  }
  bool operator<(const SyntaxKey& other) const {
    return fileKey < other.fileKey || (fileKey == other.fileKey && ordinal < other.ordinal);
  }
};
struct Argument {
  std::optional<std::string> name;
  uint32_t position = 0;
  Span whole, expression;
  std::optional<Span> nameSpan;
  std::string originalExpression;
  bool hasExpression = true;
};
enum class SiteKind { ModuleCall, ModuleDefinition, FunctionDefinition, Assignment };
struct Site {
  SyntaxKey syntax;
  Span whole;
  std::string callable;
  std::vector<Argument> arguments;
  SiteKind kind = SiteKind::ModuleCall;
  std::optional<Span> head;
  bool wholeComplete = true;
};
struct InstanceSegment { SyntaxKey call; uint64_t invocation = 0; };
struct Instance {
  uint64_t id = 0;
  std::optional<uint64_t> parent;
  std::vector<InstanceSegment> path;
  Site call;
  std::optional<Site> definition;
  std::vector<size_t> emittedNodeIndices;
};
enum class Confidence { Unique, Multiple, Unavailable, Stale };
struct Mapping {
  Confidence confidence = Confidence::Unavailable;
  std::vector<uint64_t> candidateInstances;
  std::optional<Transform3d> localToWorld;
  bool invertible = false;
  std::string reason;
  int boundaryPolarity = 1;
};

// Mutable only during native parsing. Publication uses a const copy; no AST pointers escape.
class SyntaxTable {
public:
  explicit SyntaxTable(std::shared_ptr<const EvaluationInputs> inputs);
  void bindParseFile(const std::string& path, const std::string& parserText);
  std::optional<Span> span(const Location& location) const;
  std::optional<std::string> original(const Span& span) const;
  uint64_t record(ASTNode& node, SiteKind kind, const std::string& callable,
                  const std::vector<std::shared_ptr<Assignment>>& arguments = {});
  void updateWhole(const ASTNode& node, const Location& whole);
  const Site *site(SyntaxKey key) const;
  const Site *site(const ASTNode& node) const;
  const std::map<SyntaxKey, Site>& sites() const { return records; }
  std::shared_ptr<const EvaluationInputs> inputs() const { return capturedInputs; }
private:
  friend class ProvenanceCollector;
  SyntaxTable(const SyntaxTable& other, bool sealed);
  bool sealed = false;
  std::shared_ptr<const EvaluationInputs> capturedInputs;
  std::map<std::string, std::string> aliases;
  // Per-snapshot identities, never shared between evaluations. Sealed copies only read this map.
  mutable std::map<std::string, std::string> resolvedPaths;
  std::string fileKey(const std::filesystem::path& path) const;
  std::map<SyntaxKey, Site> records;
  std::map<std::string, uint64_t> ordinals;
  mutable std::map<std::string, std::vector<uint64_t>> lineStarts;
  const SourceInput *source(const Location& location) const;
  std::optional<uint64_t> offset(const SourceInput& input, int line, int column) const;
};

extern thread_local SyntaxTable *nativeSyntaxTable;
class ParseScope {
public:
  explicit ParseScope(SyntaxTable *table);
  ~ParseScope();
  ParseScope(const ParseScope&) = delete;
  ParseScope& operator=(const ParseScope&) = delete;
private:
  SyntaxTable *previous;
};

class ProvenanceCollector;
class Result {
public:
  Mapping forNode(size_t nodeIndex) const;
  Mapping finalGeometry() const;
  const Instance *instance(uint64_t id) const;
  const Site *syntax(SyntaxKey key) const;
  const SyntaxTable& syntaxTable() const { return *table; }
  const std::map<uint64_t, Instance>& instances() const { return invocations; }
  std::shared_ptr<const Result> withPreview(const std::vector<std::shared_ptr<CSGNode>>& roots) const;
private:
  friend class ProvenanceCollector;
  Result() = default;
  std::shared_ptr<const SyntaxTable> table;
  std::map<uint64_t, Instance> invocations;
  std::map<size_t, Mapping> nodes;
  Mapping finalMapping;
};
}
