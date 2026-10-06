#pragma once
#include "core/semantic/SemanticScene.h"
#include <functional>
class EvaluationSession;
class SourceFileCache;
class Context;
class FileContext;
namespace Semantic {
struct DefaultEvaluation {
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceFileCache> libraries;
  std::shared_ptr<SourceProvenance::SyntaxTable> syntax;
  std::function<void(Context&)> configurationLifetime;
  std::shared_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> lexical;
  std::shared_ptr<const Scene> scene;
  std::shared_ptr<AbstractNode> root;
  std::string diagnostic;
  bool valid() const { return root && scene && scene->valid(); }
};
// Owns a separate native session. Evaluates the exact retained declaration, with empty arguments.
// Native global/$preview configuration is copied by the supplied callback before lexical initialization.
DefaultEvaluation evaluateDefaults(const Scene&, const std::string& definitionId,
                                  const std::function<void(Context&)>& configure = {});
}
