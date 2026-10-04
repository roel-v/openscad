#pragma once
#include "core/design/DesignObjects.h"
#include <map>
#include <memory>
#include <string>

class Expression;
class Assignment;
namespace Semantic {
enum class DefinitionKind { Part, Assembly };
struct Metadata {
  bool marked = false;
  std::string kind, error;
  std::map<std::string, std::shared_ptr<const Expression>> fields;
};
struct DefinitionPayload : Design::Payload { DefinitionKind kind; };
struct ComponentPayload : Design::Payload {};
Metadata metadata(const Expression *attributes, Design::DeclarationHost host);
Design::DecodeResult decodeDefinition(const Value&, const Design::DeclarationContext&, DefinitionKind);
Design::DecodeResult decodeComponent(const Value&, const Design::DeclarationContext&);
void registerKinds();
}
