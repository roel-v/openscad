#pragma once
#include "core/semantic/SemanticKinds.h"
#include "core/provenance/SourceProvenance.h"
#include <map>
#include <set>
#include <vector>
class SourceFile;
class UserModule;
class AbstractNode;
class ModuleInstantiation;
class EvaluationInputs;
namespace Semantic {
struct Definition {
  std::string id, label, moduleName;
  DefinitionKind kind = DefinitionKind::Part;
  Design::DeclarationKey source;
  std::optional<SourceProvenance::Site> syntax;
  std::shared_ptr<const UserModule> module;
  std::shared_ptr<const SourceFile> lexicalFile;
};
struct Parameter {
  std::string name, displayedValue;
  Value::Type type = Value::Type::UNDEFINED;
  std::shared_ptr<const Value> value;
  std::optional<size_t> winningArgument;
  std::optional<SourceProvenance::Argument> source;
  bool fromDefault = false, ambiguous = false;
};
struct Instance {
  std::string id, label, path, parentPath, definitionId;
  uint64_t invocationId = 0;
  Design::DeclarationKey source;
  std::optional<SourceProvenance::Site> call;
  std::map<std::string, Parameter> parameters;
  std::shared_ptr<const AbstractNode> localRoot;
  Transform3d originalLocalToParent = Transform3d::Identity();
  Transform3d localToParent = Transform3d::Identity();
  Transform3d localToWorld = Transform3d::Identity();
  bool displayed = false, background = false, highlight = false;
};
struct Piece {
  std::string instancePath;
  std::shared_ptr<const AbstractNode> root;
  Transform3d localToWorld = Transform3d::Identity();
  bool background = false, highlight = false;
  Color4f inheritedColor;
};
struct Scene {
  bool marked = false;
  std::string documentPath;
  std::string defaultDefinitionId; // Empty for a live document evaluation.
  std::map<std::string, Definition> definitions;
  std::map<std::string, Instance> instances;
  std::map<SourceProvenance::SyntaxKey, std::vector<std::string>> callInstances;
  std::map<size_t, std::string> partOwners;
  std::vector<Piece> parts, residual;
  std::vector<std::shared_ptr<const ModuleInstantiation>> syntheticCalls;
  std::vector<std::shared_ptr<const SourceFile>> sourceFiles;
  std::shared_ptr<const EvaluationInputs> inputs;
  std::shared_ptr<const SourceProvenance::SyntaxTable> syntax;
  std::map<std::string, std::shared_ptr<const Design::Payload>> extensions;
  std::vector<Design::Diagnostic> diagnostics;
  std::set<std::string> invalidIds;
  bool valid() const { return diagnostics.empty() && invalidIds.empty(); }
};
std::string escapeSegment(const std::string&);
std::optional<std::vector<std::string>> pathSegments(const std::string&);
}
