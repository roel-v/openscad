#include "gui/evaluation/EvaluationPipeline.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/EvaluationSession.h"
#include "core/Context.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/Tree.h"
#include "core/CSGNode.h"
#include "core/CSGTreeEvaluator.h"
#include "core/customizer/CommentParser.h"
#include "core/parsersettings.h"
#include "core/progress.h"
#include "geometry/GeometryEvaluator.h"
#include "glview/preview/CSGTreeNormalizer.h"
#include "glview/TriangleStatistics.h"
#include "gui/annotations/AgentModel.h"
#include "RenderStatistic.h"
#include "gui/ScadApi.h"
#include "utils/ScopedOutput.h"
#include "utils/exceptions.h"
#include <filesystem>
#include <stdexcept>
#ifdef ENABLE_PYTHON
#include "python/python_public.h"
#endif
#ifdef ENABLE_MANIFOLD
#include "geometry/manifold/ManifoldGeometry.h"
#endif

extern bool parse(SourceFile *&, const std::string&, const std::string&, const std::string&, int);
namespace BackgroundEvaluation {
namespace {
void diagnostic(const Message& message, void *data)
{
  auto& result = *static_cast<Result *>(data);
  std::lock_guard<std::mutex> lock(result.diagnosticMutex);
  if (message.group == message_group::Error || message.group == message_group::Parser_Error) ++result.errors;
  if (message.group == message_group::Warning || message.group == message_group::Deprecated) ++result.warnings;
  // Bound memory even for an infinite echo loop; counts remain authoritative.
  if (result.diagnostics.size() < 2048) {
    auto bounded = message;
    if (bounded.msg.size() > 8192) bounded.msg.resize(8192);
    result.diagnostics.push_back(std::move(bounded));
  } else ++result.omittedMessages;
}
struct ProgressScope {
  explicit ProgressScope(const std::shared_ptr<AbstractNode>& root) {
    progress_report_prep(root, [](const auto&, void *, int) { evaluationCheckpoint(); }, nullptr);
  }
  ~ProgressScope() { progress_report_fin(); }
};
}
void evaluate(const Request& request, Result& result, Job& job)
{
  RenderStatistic statistics;
  const auto started = std::chrono::steady_clock::now();
#ifdef ENABLE_PYTHON
  struct PythonScope {
    bool active;
    explicit PythonScope(bool active) : active(active) { if (active) python_lock(); }
    ~PythonScope() { if (active) python_unlock(); }
  } pythonScope(request.python);
#endif
  auto stage = [&](const char *name) {
    evaluationCheckpoint();
    result.timings[name] = double(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count()) / 1000.0;
    job.setStage(QString::fromLatin1(name));
  };
  ScopedOutput output(diagnostic, &result);
  FeatureSnapshotScope features(request.features);
  resetSuppressedMessages();
  initialize_rng();
  OpenSCAD::hardwarnings = request.hardWarnings;
  OpenSCAD::traceDepth = request.traceDepth;
  OpenSCAD::traceUsermoduleParameters = request.traceParameters;
  OpenSCAD::parameterCheck = request.parameterCheck;
  OpenSCAD::rangeCheck = request.rangeCheck;
  auto settings = request.settings;
  RenderSettings::Scope renderSettings(settings);
  stage("Capturing inputs");
  auto rootInput = request.root;
  auto editors = request.editors;
  auto canonicalize = [](SourceInput& source) {
    if (source.fileKey.rfind("session:", 0) != 0)
      source.fileKey = EvaluationInputs::fileKey(std::filesystem::path(source.fileKey));
  };
  canonicalize(rootInput);
  for (auto& input : editors) { evaluationCheckpoint(); canonicalize(input); }
  result.backing = std::make_shared<Selection::EvaluationBacking>();
  auto& backing = *result.backing;
  backing.inputs = std::make_shared<EvaluationInputs>(std::move(rootInput), std::move(editors));
  if (request.validity) {
    request.validity->consume(backing.inputs->root());
    backing.inputs->observeReads([validity = request.validity](const SourceInput& input) { validity->consume(input); });
  }
  backing.libraries = std::make_shared<SourceFileCache>(backing.inputs.get());
  NativeEvaluationScope scope(*backing.inputs, *backing.libraries);
  auto fulltext = request.root.utf8 + "\n\x03\n" + request.definitions;
#ifdef ENABLE_PYTHON
  if (request.python) {
    stage("Evaluating Python");
    initPython(request.pythonEnvironment, request.variables.time);
    const auto error = evaluatePython(request.root.utf8, false);
    evaluationCheckpoint();
    if (!error.empty()) throw std::runtime_error(error);
    fulltext = "\n";
  }
#endif
  stage("Parsing");
  SourceFile *parsed = nullptr;
  bool parsedOK = false;
  try {
    parsedOK = parse(parsed, fulltext, request.filename, request.filename, false);
  } catch (...) {
    backing.parsedRoot.reset(parsed);
    throw;
  }
  backing.parsedRoot.reset(parsed);
  result.parserError = parser_error_pos;
  evaluationCheckpoint(); // Lexer returns EOF on cancellation so Bison can unwind normally.
  if (!parsedOK || !parsed) return;
  stage("Loading dependencies");
  parsed->handleDependencies();
  evaluationCheckpoint();
  CommentParser::collectParameters(fulltext, parsed);
  if (request.completionMode == "ParsedFileMode")
    result.completion = ScadApi::completionFromSource(parsed, request.completionVariables,
                                                     request.completionModules, request.completionFunctions);
  else if (request.completionMode == "RegexInputTextMode")
    result.completion = ScadApi::completionFromText(QString::fromUtf8(request.root.utf8.data(), int(request.root.utf8.size())),
      request.completionVariables, request.completionModules, request.completionFunctions);
  stage("Evaluating parameters");
  const auto directory = std::filesystem::path(request.filename).parent_path().string();
  {
    EvaluationSession session(directory);
    ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(&session)};
    request.variables.applyToContext(builtin);
    ContextHandle<FileContext> context{Context::create<FileContext>(*builtin, parsed)};
    result.parameters = ParameterObjects::fromSourceFile(parsed, context.operator->());
    result.parameters.importValues(request.parameters);
    for (const auto& parameter : result.parameters) {
      evaluationCheckpoint(); parameter->updateContext(context.operator->());
    }
    for (const auto& parameter : result.parameters) {
      evaluationCheckpoint(); parameter->updateAttributes(context.operator->());
    }
    result.parameters.apply(parsed);
  }
  if (result.errors) return;
  if (request.mode != Mode::Parameters) {
    stage("Evaluating language");
    AbstractNode::resetIndexCounter();
    backing.session = std::make_shared<EvaluationSession>(directory);
    ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(backing.session.get())};
    backing.builtin = *builtin;
    request.variables.applyToContext(builtin);
#ifdef ENABLE_PYTHON
    if (request.python) result.absoluteRoot = python_result_node;
    else
#endif
      result.absoluteRoot = parsed->instantiate(*builtin, &backing.file);
    const Location *otherRoot = nullptr;
    result.root = result.absoluteRoot ? find_root_tag(result.absoluteRoot, &otherRoot) : nullptr;
    if (!result.root) result.root = result.absoluteRoot;
    if (otherRoot) LOG(message_group::NONE, *otherRoot, builtin->documentRoot(),
                       "More than one Root Modifier (!)");
    if (!result.root || result.errors) return;
    result.camera = request.variables.camera;
    if (backing.file) result.camera.updateView(backing.file, false);
    Tree tree(result.root, directory);
    ProgressScope progress(result.root);
    GeometryEvaluator geometry(tree);
    if (request.mode == Mode::Render) {
      stage("Rendering geometry");
      result.geometry = geometry.evaluateGeometry(*result.root, true);
#ifdef ENABLE_MANIFOLD
      if (auto manifold = std::dynamic_pointer_cast<const ManifoldGeometry>(result.geometry)) {
        if (manifold->getManifold().Status() != manifold::Manifold::Error::NoError)
          throw std::runtime_error("Manifold evaluation failed");
      }
#endif
      stage("Preparing render geometry");
#ifndef USE_POLYSET_FOR_CGAL
      result.usePolySet = request.settings.backend3D == RenderBackend3D::ManifoldBackend ||
        std::dynamic_pointer_cast<const PolySet>(result.geometry) ||
        std::dynamic_pointer_cast<const Polygon2d>(result.geometry);
#endif
      if (result.usePolySet) result.preparedGeometry = PolySetRenderer::prepareGeometry(result.geometry);
#ifndef USE_POLYSET_FOR_CGAL
      else result.preparedCGAL = CGALRenderer::prepareGeometry(result.geometry, request.colors);
#endif
      if (result.geometry) statistics.printAll(result.geometry, result.camera, request.summary);
    } else {
      stage("Generating CSG");
      CSGTreeEvaluator csg(tree, &geometry);
      result.csg = csg.buildCSGTree(*result.root);
      stage("Normalizing CSG");
      CSGTreeNormalizer normalizer(2 * request.csgLimit);
      auto products = [&](const auto& nodes) {
        auto output = std::make_shared<CSGProducts>();
        for (const auto& node : nodes) {
          evaluationCheckpoint();
          if (auto normalized = normalizer.normalize(node)) output->import(normalized);
        }
        return output;
      };
      result.normalized = result.csg ? normalizer.normalize(result.csg) : nullptr;
      result.products = std::make_shared<CSGProducts>();
      if (result.normalized) result.products->import(result.normalized);
      result.highlights = products(csg.getHighlightNodes());
      result.background = products(csg.getBackgroundNodes());
      stage("Preparing preview statistics");
      result.previewTriangles = previewTriangleStatistics(result.products, result.highlights, result.background);
    }
    stage("Preparing inspection");
    result.inspection = AgentAnnotations::inspectModel(result.absoluteRoot.get(), result.root.get(), parsed,
                                                     request.metadata);
    result.scene = std::make_shared<Selection::EvaluatedScene>();
    result.scene->backing = result.backing;
    result.scene->inputs = backing.inputs;
    result.scene->root = result.root;
    result.scene->documentPath = directory;
  }
  backing.inputs->freeze();
  stage("Checking inputs");
  for (const auto& [key, input] : backing.inputs->consumed()) {
    evaluationCheckpoint();
    if (!input.editorId && EvaluationInputs::fingerprint(EvaluationInputs::diskBytes(key)) != input.fingerprint)
      throw std::runtime_error("An evaluation dependency changed during computation");
  }
  result.success = result.errors == 0;
  stage("Ready");
}
}
