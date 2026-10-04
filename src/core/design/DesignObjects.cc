#include "core/design/DesignObjects.h"
#include <stdexcept>

namespace Design {
Value ownedValue(const Value& value)
{
  if (value.type() == Value::Type::OBJECT) {
    ObjectType result(nullptr);
    for (const auto& key : value.toObject().keys()) result.set(key, ownedValue(value.toObject().get(key)));
    return Value(std::move(result));
  }
  if (value.type() == Value::Type::VECTOR || value.type() == Value::Type::EMBEDDED_VECTOR) {
    VectorType result(nullptr);
    for (const auto& item : value.toVector()) result.emplace_back(ownedValue(item));
    return Value(std::move(result));
  }
  if (value.type() == Value::Type::FUNCTION || value.type() == Value::Type::RANGE)
    throw std::invalid_argument("Design definitions must contain immutable data values");
  return value.clone();
}

std::shared_ptr<const Record> makeRecord(const KindHandler& handler, const Value& value,
                                        const DeclarationContext& declaration,
                                        std::vector<Diagnostic>& diagnostics)
{
  auto fail = [&](const std::string& code, const std::string& message) {
    diagnostics.push_back({code, message, declaration.source});
    return std::shared_ptr<const Record>();
  };
  if (!handler.hosts.count(declaration.source.host) || !handler.scopes.count(declaration.scope))
    return fail("unsupported-host-scope", "Kind does not permit this declaration host or scope");
  if (value.type() != Value::Type::OBJECT) return fail("object-required", "Design declaration must evaluate to an object");
  const auto& object = value.toObject();
  const auto& id = object.get("id");
  if (id.type() != Value::Type::STRING || id.toString().empty())
    return fail("id-required", "Design id must be a nonempty string");
  if (object.contains("label") && object.get("label").type() != Value::Type::STRING)
    return fail("invalid-label", "Design label must be a string");
  auto decoded = handler.decode(value, declaration);
  for (auto& diagnostic : decoded.diagnostics) {
    if (!diagnostic.source) diagnostic.source = declaration.source;
    diagnostics.push_back(std::move(diagnostic));
  }
  if (!decoded.payload) return {};
  auto result = std::make_shared<Record>();
  result->id = id.toString();
  result->kind = handler.name;
  result->label = object.contains("label") ? object.get("label").toString() : declaration.source.hostName;
  try { result->definition = ownedValue(value); }
  catch (const std::invalid_argument& error) { return fail("non-data-definition", error.what()); }
  result->evaluated = std::move(decoded.payload);
  result->dependencies = std::move(decoded.dependencies);
  result->source = declaration.source;
  return result;
}
}
