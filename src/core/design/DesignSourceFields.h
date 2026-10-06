#pragma once
#include "core/design/DesignObjects.h"
#include "core/EvaluationInputs.h"

namespace Design {
struct SourceRange { size_t begin = 0, end = 0; };
// Requires syntactically valid source. Returns a safe file-scope insertion point.
std::optional<size_t> fileScopeInsertion(const std::string& source, size_t cursor, std::string& error);
struct SourcePatch { SourceRange range; std::string expected, replacement; };
enum class LiteralKind { String, Number, Vector3, Quaternion };
struct FieldPatch { std::string path, encoded; LiteralKind literal; };
struct ObjectFields {
  SourceRange range;
  std::map<std::string, SourceRange> fields;
  size_t close = 0;
  bool trailingComma = false, multiline = false;
  std::string indent, eol = "\n";
};
class DesignSourceMap {
public:
  static DesignSourceMap scan(const DeclarationKey&, const SourceInput&, const std::string& expectedKind);
  bool direct() const { return problem.empty(); }
  const std::string& error() const { return problem; }
  bool ownsDeclaration() const { return owned; }
  const ObjectFields& fields() const { return root; }
  std::optional<std::vector<SourcePatch>> patches(const std::vector<FieldPatch>&, std::string& error,
      const std::set<std::string>& allowedFields = {"label","adjust.translation","adjust.rotation","adjust.offset"}) const;
  std::optional<SourcePatch> deletion(std::string& error) const;
private:
  std::string bytes, problem;
  DeclarationKey declaration;
  ObjectFields root;
  std::optional<ObjectFields> adjust;
  bool owned = false;
};
}
