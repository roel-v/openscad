#pragma once
#include "gui/evaluation/EvaluationService.h"
#include "gui/selection/EvaluationBinding.h"
#include "core/RenderVariables.h"
#include "core/customizer/ParameterObject.h"
#include "glview/RenderSettings.h"
#include "glview/PolySetRenderer.h"
#include "glview/cgal/CGALRenderer.h"
#include "gui/annotations/AgentSyntax.h"
#include "utils/printutils.h"
#include <QJsonObject>
#include <QStringList>
#include <chrono>
#include <mutex>
#include <set>
#include "Feature.h"

class CSGNode;
class CSGProducts;
class Geometry;
namespace BackgroundEvaluation {
enum class Mode { Parameters, Preview, Render };
struct InputValidity {
  std::mutex mutex;
  std::set<std::string> used, modified;
  EvaluationCancellation cancellation;
  void consume(const SourceInput& input) {
    if (!input.editorId) return;
    std::lock_guard<std::mutex> lock(mutex);
    used.insert(*input.editorId);
    if (modified.count(*input.editorId)) cancellation->store(true);
  }
  void invalidate(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex);
    modified.insert(id);
    if (used.count(id)) cancellation->store(true);
  }
};
struct Request {
  SourceInput root;
  std::vector<SourceInput> editors;
  std::string filename, definitions;
#ifdef ENABLE_PYTHON
  bool python = false;
  std::string pythonEnvironment;
#endif
  ParameterSet parameters;
  bool strictParameters = false;
  // Captured on the GUI thread. Workers must never consult preferences/widgets.
  bool prepareInspection = false;
  RenderVariables variables{};
  RenderSettings settings = *RenderSettings::inst();
  QJsonObject metadata;
  FeatureSnapshot features;
  ColorScheme colors;
  std::vector<std::string> summary;
  std::shared_ptr<InputValidity> validity;
  QString completionMode;
  bool completionVariables = false, completionModules = false, completionFunctions = false;
  Mode mode = Mode::Preview;
  bool hardWarnings = false, parameterCheck = true, rangeCheck = false, traceParameters = true;
  unsigned traceDepth = 12;
  size_t csgLimit = 100000;
};
struct Result {
  std::shared_ptr<Selection::EvaluationBacking> backing;
  std::shared_ptr<Selection::EvaluatedScene> scene;
  std::shared_ptr<AbstractNode> absoluteRoot, root;
  std::shared_ptr<CSGNode> csg, normalized;
  std::shared_ptr<CSGProducts> products, highlights, background;
  std::shared_ptr<const Geometry> geometry;
  PolySetRenderer::PreparedGeometry preparedGeometry;
  Renderer::TriangleStatistics previewTriangles{Renderer::TriangleStatistics::Kind::Preview, std::nullopt};
  CGALRenderer::PreparedGeometry preparedCGAL;
  bool usePolySet = true;
  ParameterObjects parameters;
  AgentAnnotations::InspectionTree inspection;
  Camera camera;
  bool success = false;
  int parserError = -1, errors = 0, warnings = 0;
  size_t omittedMessages = 0;
  std::mutex diagnosticMutex;
  std::vector<Message> diagnostics;
  QJsonObject timings;
  QStringList completion;
};
void evaluate(const Request& request, Result& result, Job& job);
}
