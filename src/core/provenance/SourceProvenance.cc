#include "core/provenance/SourceProvenance.h"
#include "core/AST.h"
#include "core/Assignment.h"
#include "core/EvaluationInputs.h"
#include "core/Expression.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace SourceProvenance {
namespace {
std::vector<ModelRequirement>& modelRequirements() {
  static std::vector<ModelRequirement> requirements;
  return requirements;
}
}
bool registerModelRequirement(ModelRequirement requirement) {
  modelRequirements().push_back(requirement);
  return true;
}
bool modelRequiresInspection(const SourceFile& root) {
  return std::any_of(modelRequirements().begin(), modelRequirements().end(),
    [&root](auto required) { return required(root); });
}
thread_local SyntaxTable *nativeSyntaxTable = nullptr;
ParseScope::ParseScope(SyntaxTable *table) : previous(nativeSyntaxTable) { nativeSyntaxTable = table; }
ParseScope::~ParseScope() { nativeSyntaxTable = previous; }

SyntaxTable::SyntaxTable(std::shared_ptr<const EvaluationInputs> inputs) : capturedInputs(std::move(inputs)) {
  if (!capturedInputs) throw std::invalid_argument("Provenance requires captured evaluation inputs");
}
SyntaxTable::SyntaxTable(const SyntaxTable& other, bool sealed)
  : sealed(sealed), capturedInputs(other.capturedInputs), aliases(other.aliases), resolvedPaths(other.resolvedPaths), records(other.records),
    overrides(other.overrides), ordinals(other.ordinals), lineStarts(other.lineStarts) {
  for (const auto& [key, input] : capturedInputs->consumed()) {
    if (lineStarts.count(key)) continue;
    auto& starts = lineStarts[key]; starts.push_back(0);
    for (size_t i = 0; i < input.utf8.size(); ++i) if (input.utf8[i] == '\n') starts.push_back(i+1);
  }
}
std::string SyntaxTable::fileKey(const std::filesystem::path& path) const {
  // Absolute lexical paths distinguish relative names across parser working directories without I/O.
  const auto absolute = std::filesystem::absolute(path).generic_string();
  if (auto found = resolvedPaths.find(absolute); found != resolvedPaths.end()) return found->second;
  auto key = EvaluationInputs::fileKey(absolute);
  if (!sealed) resolvedPaths.emplace(absolute, key);
  return key;
}
void SyntaxTable::bindParseFile(const std::string& path, const std::string& parserText) {
  if (sealed) throw std::logic_error("Published provenance syntax is immutable");
  const auto key = fileKey(path);
  const auto& consumed = capturedInputs->consumed();
  auto found = consumed.find(key);
  const SourceInput *input = found == consumed.end() ? nullptr : &found->second;
  // Untitled native documents use the parser's directory as their diagnostic path.
  if (!input && capturedInputs->root().fileKey.rfind("session:", 0) == 0) input = &capturedInputs->root();
  if (!input || parserText.compare(0, input->utf8.size(), input->utf8) != 0) return;
  const auto suffix = parserText.substr(input->utf8.size());
  if (!suffix.empty() && suffix.rfind("\n\x03\n", 0) != 0) return;
  aliases[key] = input->fileKey;
}
const SourceInput *SyntaxTable::source(const Location& location) const {
  if (location.isNone() || location.fileName().empty()) return nullptr;
  auto key = fileKey(location.filePath());
  auto alias = aliases.find(key);
  if (alias != aliases.end()) key = alias->second;
  const auto found = capturedInputs->consumed().find(key);
  return found == capturedInputs->consumed().end() ? nullptr : &found->second;
}
std::optional<uint64_t> SyntaxTable::offset(const SourceInput& input, int line, int column) const {
  if (line < 1 || column < 1) return {};
  auto entry = lineStarts.find(input.fileKey);
  if (entry == lineStarts.end()) {
    if (sealed) return {};
    entry = lineStarts.emplace(input.fileKey, std::vector<uint64_t>{}).first;
    entry->second.push_back(0);
    for (size_t i = 0; i < input.utf8.size(); ++i)
      if (input.utf8[i] == '\n') entry->second.push_back(i + 1);
  }
  const auto& starts = entry->second;
  if (static_cast<size_t>(line) > starts.size()) return {};
  const auto begin = starts[line - 1];
  const auto limit = static_cast<size_t>(line) < starts.size() ? starts[line] : input.utf8.size();
  const uint64_t position = begin + static_cast<uint64_t>(column - 1);
  if (position > limit || position > input.utf8.size()) return {};
  if (position < input.utf8.size() && (static_cast<unsigned char>(input.utf8[position]) & 0xc0) == 0x80) return {};
  return position;
}
std::optional<Span> SyntaxTable::span(const Location& location) const {
  const auto *input = source(location);
  if (!input) return {};
  const auto begin = offset(*input, location.firstLine(), location.firstColumn());
  const auto end = offset(*input, location.lastLine(), location.lastColumn());
  if (!begin || !end || *begin > *end) return {};
  return Span{input->fileKey, input->fingerprint, *begin, *end};
}
std::optional<std::string> SyntaxTable::original(const Span& range) const {
  const auto found = capturedInputs->consumed().find(range.fileKey);
  if (found == capturedInputs->consumed().end()) return {};
  const auto& input = found->second;
  if (input.fingerprint != range.fingerprint || range.begin > range.end || range.end > input.utf8.size()) return {};
  for (auto position : {range.begin, range.end})
    if (position < input.utf8.size() && (static_cast<unsigned char>(input.utf8[position]) & 0xc0) == 0x80) return {};
  return input.utf8.substr(range.begin, range.end - range.begin);
}
uint64_t SyntaxTable::record(ASTNode& node, SiteKind kind, const std::string& callable,
                            const std::vector<std::shared_ptr<Assignment>>& arguments) {
  if (sealed) throw std::logic_error("Published provenance syntax is immutable");
  auto whole = span(node.location());
  if (!whole) return 0; // Synthetic command-line or unavailable source cannot become editable.
  Site site{{whole->fileKey, ++ordinals[whole->fileKey]}, *whole, callable, {}, kind};
  if (kind == SiteKind::ModuleCall || kind == SiteKind::ModuleDefinition) {
    site.head = *whole;
    site.whole = {};
    site.wholeComplete = false;
  }
  if (arguments.size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("Too many provenance arguments");
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& assignment = arguments[i];
    auto range = span(assignment->location());
    if (!range) continue;
    Argument argument;
    argument.position = static_cast<uint32_t>(i);
    argument.whole = *range;
    argument.hasExpression = !!assignment->getExpr();
    if (!assignment->getName().empty()) {
      argument.name = assignment->getName();
      argument.nameSpan = span(assignment->sourceNameLocation());
    }
    if (argument.hasExpression) {
      auto expression = span(assignment->sourceExpressionLocation());
      if (!expression) continue;
      argument.expression = *expression;
      argument.originalExpression = *original(*expression);
    } else {
      argument.expression = Span{range->fileKey, range->fingerprint, range->end, range->end};
    }
    site.arguments.push_back(std::move(argument));
  }
  const auto ordinal = site.syntax.ordinal;
  store(std::move(site));
  node.setSyntaxOrdinal(ordinal);
  return ordinal;
}
const Site *SyntaxTable::site(SyntaxKey key) const {
  if (const auto changed = overrides.find(key); changed != overrides.end()) return &changed->second;
  const auto file = records.find(key.fileKey);
  if (file == records.end()) return nullptr;
  const auto found = file->second->find(key);
  return found == file->second->end() ? nullptr : &found->second;
}
void SyntaxTable::store(Site value) {
  const auto key = value.syntax;
  // A provisional module head differs from its completed span while parsing.
  // Keep that write local; once completed, identical records use the base again.
  if (const auto current = site(key); current && *current == value) return;
  allRecords.reset();
  auto& file = records[key.fileKey];
  if (!file) file = std::make_shared<Records>();
  const auto original = file->find(key);
  if (original != file->end() && original->second == value) {
    overrides.erase(key);
  } else if (file.use_count() == 1) {
    overrides.erase(key);
    file->insert_or_assign(key, std::move(value));
  } else {
    overrides.insert_or_assign(key, std::move(value));
  }
}
const std::map<SyntaxKey, Site>& SyntaxTable::sites() const {
  if (!allRecords) {
    auto combined = std::make_shared<Records>();
    for (const auto& [key, file] : records) combined->insert(file->begin(), file->end());
    for (const auto& [key, value] : overrides) combined->insert_or_assign(key, value);
    allRecords = std::move(combined);
  }
  return *allRecords;
}
std::shared_ptr<SyntaxTable> SyntaxTable::reuseForParse(std::shared_ptr<const EvaluationInputs> inputs) const {
  if (!inputs || inputs->root().fileKey != capturedInputs->root().fileKey)
    throw std::invalid_argument("Syntax reuse requires the same captured root");
  for (const auto& [key, input] : capturedInputs->consumed()) {
    const auto current = inputs->consumed().find(key);
    if (current == inputs->consumed().end() || current->second.fingerprint != input.fingerprint)
      throw std::invalid_argument("Syntax reuse requires identical captured inputs");
  }
  auto result = std::make_shared<SyntaxTable>(std::move(inputs));
  result->records = records;
  result->overrides = overrides;
  result->aliases = aliases;
  result->resolvedPaths = resolvedPaths;
  result->lineStarts = lineStarts;
  // The unchanged root is parsed again into a new, parameter-mutable AST. It
  // receives the same ordinals; retained library ASTs keep their original IDs.
  return result;
}
const Site *SyntaxTable::site(const ASTNode& node) const {
  const auto *input = source(node.location());
  return input ? site({input->fileKey, node.syntaxOrdinal()}) : nullptr;
}
void SyntaxTable::updateWhole(const ASTNode& node, const Location& whole) {
  if (sealed) throw std::logic_error("Published provenance syntax is immutable");
  const auto *input = source(node.location());
  auto range = span(whole);
  if (!input) return;
  const SyntaxKey key{input->fileKey, node.syntaxOrdinal()};
  const auto previous = site(key);
  if (!previous) return;
  const bool complete = range && range->fileKey == input->fileKey;
  const auto updated = complete ? *range : Span{};
  if (previous->wholeComplete == complete && previous->whole == updated) return;
  allRecords.reset();
  auto& file = records.at(input->fileKey);
  if (auto changed = overrides.find(key); changed != overrides.end()) {
    changed->second.wholeComplete = complete;
    changed->second.whole = updated;
    const auto original = file->find(key);
    if (original != file->end() && original->second == changed->second) overrides.erase(changed);
    return;
  }
  if (file.use_count() == 1) {
    auto& value = file->at(key);
    value.wholeComplete = complete;
    value.whole = updated;
    return;
  }
  Site value = *previous;
  value.wholeComplete = complete;
  value.whole = updated;
  store(std::move(value));
}
Mapping Result::forNode(size_t nodeIndex) const {
  auto found = nodes.find(nodeIndex);
  if (found != nodes.end()) return found->second;
  return {Confidence::Unavailable, {}, {}, false, "No source invocation owns this evaluated node"};
}
Mapping Result::finalGeometry() const { return finalMapping; }
const Instance *Result::instance(uint64_t id) const {
  auto found = invocations.find(id);
  return found == invocations.end() ? nullptr : &found->second;
}
const Site *Result::syntax(SyntaxKey key) const { return table ? table->site(std::move(key)) : nullptr; }
}
