#include "core/semantic/SemanticKinds.h"
#include "core/semantic/SemanticCollector.h"
#include "core/design/DesignKinds.h"
#include "core/EvaluationSession.h"
#include "core/Expression.h"
#include <mutex>
#include <stdexcept>

namespace Semantic {
Metadata metadata(const Expression *attributes, Design::DeclarationHost host) {
  if (!attributes) return {};
  Metadata result; result.marked = true;
  const auto *object = dynamic_cast<const ObjectExpression *>(attributes);
  if (!object) { result.error = "Semantic metadata must be an attribute object"; return result; }
  for (const auto& field : object->getMembers()) {
    const auto& name = field->getName();
    if (name != "type" && name != "id" && name != "label") {
      result.error = "Unknown semantic metadata field: " + name; return result;
    }
    if (!result.fields.emplace(name, field->getExpr()).second) {
      result.error = "Duplicate semantic metadata field: " + name; return result;
    }
  }
  auto type = result.fields.find("type");
  const auto *lookup = type == result.fields.end() ? nullptr : dynamic_cast<const Lookup *>(type->second.get());
  if (!lookup) { result.error = "Semantic type must be a bare registered kind name"; return result; }
  result.kind = lookup->get_name();
  if ((host == Design::DeclarationHost::ModuleDefinition && result.kind != "Part" && result.kind != "Assembly") ||
      (host == Design::DeclarationHost::ModuleCall && result.kind != "Component")) {
    result.error = "Kind does not permit this semantic declaration host"; return result;
  }
  auto handler = Design::kind(result.kind);
  if (!handler) { result.error = "Unknown design kind: " + result.kind; return result; }
  if (!handler->hosts.count(host)) { result.error = "Kind does not permit this declaration host"; return result; }
  if (!result.fields.count("id")) { result.error = "Semantic id is required"; return result; }
  if (host == Design::DeclarationHost::ModuleDefinition) {
    for (const auto& name : {"id", "label"}) {
      auto field = result.fields.find(name);
      if (field == result.fields.end()) continue;
      const auto *literal = dynamic_cast<const Literal *>(field->second.get());
      if (!literal || !literal->isString() || (std::string(name) == "id" && literal->toString().empty())) {
        result.error = "Definition " + std::string(name) + " must be a " +
          (std::string(name) == "id" ? "nonempty string literal" : "string literal"); return result;
      }
    }
  }
  return result;
}
namespace {
bool dataFields(const Value& value, Design::DecodeResult& output) {
  if (value.type() != Value::Type::OBJECT) return false;
  for (const auto& name : value.toObject().keys()) if (name != "id" && name != "label") {
    output.diagnostics.push_back({"unknown-semantic-member", "Unknown semantic member: " + name, {}});
    return false;
  }
  return true;
}
}
Design::DecodeResult decodeDefinition(const Value& value, const Design::DeclarationContext&, DefinitionKind kind) {
  Design::DecodeResult output;
  if (dataFields(value, output)) {
    auto payload = std::make_shared<DefinitionPayload>(); payload->kind = kind; output.payload = payload;
  }
  return output;
}
Design::DecodeResult decodeComponent(const Value& value, const Design::DeclarationContext&) {
  Design::DecodeResult output;
  if (dataFields(value, output)) output.payload = std::make_shared<ComponentPayload>();
  return output;
}
void registerKinds() {
  static std::once_flag once;
  std::call_once(once, [] {
    Design::registerBuiltins();
    for (const auto& entry : {std::make_pair("Part", DefinitionKind::Part), std::make_pair("Assembly", DefinitionKind::Assembly)}) {
      if (!Design::registerKind({entry.first, {Design::DeclarationHost::ModuleDefinition}, {Design::DeclarationScope::File},
          [kind=entry.second](const Value& value, const Design::DeclarationContext& context) { return decodeDefinition(value, context, kind); }}))
        throw std::logic_error("Duplicate semantic kind registration");
    }
    if (!Design::registerKind({"Component", {Design::DeclarationHost::ModuleCall},
        {Design::DeclarationScope::File, Design::DeclarationScope::Local}, decodeComponent, Design::RecordDestination::Consumer,
        [](Design::Record record, const Design::EvaluatedDeclaration& declaration, EvaluationSession& session) {
          if (!session.semanticCollector) throw std::logic_error("Component dispatch requires an active semantic evaluation");
          session.semanticCollector->captureComponent(std::move(record), declaration);
        }})) throw std::logic_error("Duplicate Component kind registration");
  });
}
}
