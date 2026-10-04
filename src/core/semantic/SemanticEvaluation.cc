#include "core/semantic/SemanticEvaluation.h"
#include "core/semantic/SemanticCollector.h"
#include "core/design/DesignCollector.h"
#include "core/EvaluationInputs.h"
#include "core/EvaluationSession.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/BuiltinContext.h"
#include "core/ScopeContext.h"
#include "core/provenance/ProvenanceCollector.h"
#include "core/node.h"
#include <filesystem>
namespace Semantic {
DefaultEvaluation evaluateDefaults(const Scene& retained, const std::string& id, const std::function<void(Context&)>& configure) {
  DefaultEvaluation result;
  result.configurationLifetime = configure;
  auto found = retained.definitions.find(id);
  if (!retained.valid() || found == retained.definitions.end() || !retained.inputs || !retained.syntax || retained.sourceFiles.empty()) { result.diagnostic = "Definition or captured evaluation input is unavailable"; return result; }
  const auto& target = found->second;
  try {
    // Copy includes the complete consumed map and its frozen state; no current disk read is permitted.
    result.inputs = std::make_shared<EvaluationInputs>(*retained.inputs); result.inputs->freeze();
    result.libraries = std::make_shared<SourceFileCache>(result.inputs.get());
    result.syntax = std::make_shared<SourceProvenance::SyntaxTable>(*retained.syntax);
    NativeEvaluationScope native(*result.inputs, *result.libraries); SourceProvenance::ParseScope spans(result.syntax.get());
    for (size_t i = 0; i < retained.sourceFiles.size(); ++i) {
      const auto& source = retained.sourceFiles[i];
      result.libraries->seedCaptured(i == 0 ? retained.inputs->root().fileKey : EvaluationInputs::fileKey(source->getFullpath()), *source);
    }
    result.session = std::make_shared<EvaluationSession>(retained.documentPath);
    result.session->designCollector = std::make_shared<Design::Collector>(result.inputs.get(), retained.inputs->root().fileKey);
    result.session->semanticCollector = std::make_shared<Collector>(*result.session);
    result.session->provenanceCollector = std::make_shared<SourceProvenance::ProvenanceCollector>(result.syntax);
    // Exact retained modules must be indexed; separately parsed libraries only serve native nested lookups.
    result.session->semanticCollector->discover(*retained.sourceFiles.front());
    ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(result.session.get())};
    if (configure) configure(*builtin.operator->());
    result.builtin = *builtin;
    ContextHandle<FileContext> lexical{Context::create<FileContext>(result.builtin, target.lexicalFile.get(), true)};
    result.lexical = *lexical;
    result.root = result.session->semanticCollector->instantiateDefaults(target, result.lexical);
    result.scene = result.session->semanticCollector->finish();
    if (!result.valid()) result.diagnostic = result.scene->diagnostics.empty() ? "Definition defaults failed" : result.scene->diagnostics.front().message;
  } catch (const std::exception& error) { result.root.reset(); result.diagnostic = error.what(); }
  return result;
}
}
