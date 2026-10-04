#pragma once
#include "core/design/DesignObjects.h"
#include "core/EvaluationInputs.h"
#include <memory>

class Assignment;
class LocalScope;
class Context;
namespace Design {
struct Metadata { bool marked = false; std::string kind, error; };
Metadata metadata(const Assignment&);
class Collector {
public:
  explicit Collector(const EvaluationInputs *inputs, std::string parserRoot = {});
  void collect(const Context&, const LocalScope&, DeclarationScope, bool useLibrary = false);
  void dispatch(const std::string& kind, const Value&, const EvaluatedDeclaration&, EvaluationSession&);
  std::shared_ptr<const Result> finish() const;
private:
  const EvaluationInputs *inputs;
  std::string parserRoot;
  std::set<const Assignment *> invalidLocal;
  void validateLocalDeclarations(const LocalScope&, bool file = true);
  Result result;
  std::optional<DeclarationKey> sourceKey(const Assignment&) const;
};
}
