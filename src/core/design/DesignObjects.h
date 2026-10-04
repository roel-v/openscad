#pragma once
#include "core/Value.h"
#include "geometry/linalg.h"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class Context;
class LocalScope;
class EvaluationSession;

namespace Design {
struct ReferenceToken {
  std::string domain, id;
  Value data = Value::undefined.clone();
};
enum class ResolutionState { Resolved, Changed, Missing, Unsupported, Invalid };
struct Resolution {
  ResolutionState state = ResolutionState::Invalid;
  std::optional<Transform3d> frame;
  std::optional<BoundingBox> bounds;
  std::string diagnostic;
};
struct Payload { virtual ~Payload() = default; };
enum class DeclarationHost { Assignment, ModuleDefinition, ModuleCall };
struct DeclarationKey {
  std::string fileKey, fingerprint, hostName;
  DeclarationHost host = DeclarationHost::Assignment;
  size_t beginByte = 0, endByte = 0;
};
enum class DeclarationScope { File, Local };
struct DeclarationContext { DeclarationKey source; DeclarationScope scope = DeclarationScope::File; };
enum class SourceOrigin { Root, Include, UseLibrary };
struct EvaluatedDeclaration {
  DeclarationKey source;
  DeclarationScope scope;
  const Context *nativeContext;
  const LocalScope *localScope;
  SourceOrigin origin;
};
struct Diagnostic { std::string code, message; std::optional<DeclarationKey> source; };
struct DecodeResult {
  std::shared_ptr<const Payload> payload;
  std::vector<ReferenceToken> dependencies;
  std::map<std::string, Value> canonicalFields;
  std::vector<Diagnostic> diagnostics;
};
struct Record {
  std::string id, kind, label;
  Value definition = Value::undefined.clone();
  std::shared_ptr<const Payload> evaluated;
  std::vector<ReferenceToken> dependencies;
  std::optional<DeclarationKey> source;
};
using Records = std::map<std::string, std::shared_ptr<const Record>>;
enum class RecordDestination { DocumentRegistry, Consumer };
struct KindHandler {
  std::string name;
  std::set<DeclarationHost> hosts;
  std::set<DeclarationScope> scopes;
  std::function<DecodeResult(const Value&, const DeclarationContext&)> decode;
  RecordDestination destination = RecordDestination::DocumentRegistry;
  std::function<void(Record, const EvaluatedDeclaration&, EvaluationSession&)> consume;
};
// Published definitions contain data only, with no evaluator/session bookkeeping pointers.
Value ownedValue(const Value& value);
std::shared_ptr<const Record> makeRecord(const KindHandler&, const Value&, const DeclarationContext&,
                                       std::vector<Diagnostic>&);
struct Result {
  Records records;
  std::vector<Diagnostic> diagnostics;
  std::set<std::string> invalidIds;
};
}
