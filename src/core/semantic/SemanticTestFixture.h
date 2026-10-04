#pragma once
#include "core/semantic/SemanticCollector.h"
#include "core/provenance/ProvenanceCollector.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/EvaluationInputs.h"
#include "core/EvaluationSession.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/ScopeContext.h"
#include "core/node.h"
#include "core/Tree.h"
#include "geometry/GeometryEvaluator.h"
#include "openscad.h"
#include "platform/PlatformUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
namespace SemanticTest {
struct Fixture {
  std::filesystem::path directory, root;
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceProvenance::SyntaxTable> syntax;
  std::unique_ptr<SourceFileCache> cache;
  std::unique_ptr<SourceFile> parsed;
  std::unique_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> file;
  std::shared_ptr<AbstractNode> node;
  std::shared_ptr<const Semantic::Scene> scene;
  std::shared_ptr<const SourceProvenance::Result> provenance;
  Fixture() {
    static std::atomic<unsigned> serial{0};
    directory = std::filesystem::absolute(std::filesystem::temp_directory_path() /
      ("semantic-evaluation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial)));
    REQUIRE(std::filesystem::create_directories(directory)); root = directory / "root.scad";
  }
  ~Fixture() {
    provenance.reset(); scene.reset(); node.reset(); file.reset(); builtin.reset(); session.reset(); parsed.reset(); cache.reset(); syntax.reset();
    std::error_code error; std::filesystem::remove_all(directory, error);
  }
  void write(const std::string& name, const std::string& text) { std::ofstream stream(directory / name, std::ios::binary); stream << text; REQUIRE(stream.good()); }
  void evaluate(const std::string& text) {
    static const bool initialized = [] {
      PlatformUtils::registerApplicationPath(std::filesystem::path(OPENSCAD_TEST_DATA_DIR).parent_path().parent_path().string());
      Builtins::initialize(); return true;
    }(); (void)initialized;
    write("root.scad", text);
    inputs = std::make_shared<EvaluationInputs>(SourceInput{EvaluationInputs::fileKey(root), text, {}, "semantic-fixture", 1});
    syntax = std::make_shared<SourceProvenance::SyntaxTable>(inputs); cache = std::make_unique<SourceFileCache>(inputs.get());
    NativeEvaluationScope native(*inputs, *cache); SourceProvenance::ParseScope spans(syntax.get());
    SourceFile *fileAST = nullptr; REQUIRE(parse(fileAST, text, root.string(), root.string(), false)); parsed.reset(fileAST); parsed->handleDependencies();
    session = std::make_unique<EvaluationSession>(directory.string());
    session->provenanceCollector = std::make_shared<SourceProvenance::ProvenanceCollector>(syntax);
    auto context = Context::create<BuiltinContext>(session.get()); builtin = *context;
    AbstractNode::resetIndexCounter(); node = parsed->instantiate(builtin, &file);
    scene = session->semanticCollector->finish(); inputs->freeze();
    REQUIRE(session->semanticCollector->activeDepth() == 0); REQUIRE(session->provenanceCollector->activeDepth() == 0);
    if (node) provenance = session->provenanceCollector->finish(*node);
  }
  bool diagnostic(const std::string& code) const { for (const auto& d : scene->diagnostics) if (d.code == code) return true; return false; }
  BoundingBox bounds(const std::shared_ptr<const AbstractNode>& target) {
    Tree tree(target); GeometryEvaluator evaluator(tree); auto geometry = evaluator.evaluateGeometry(*target, true); REQUIRE(geometry); return geometry->getBoundingBox();
  }
};
inline const std::string part = "@{type:Part,id:\"bracket\",label:\"Bracket\"} module bracket(width=30) {difference(){cube([width,10,3]);translate([5,5,-1]) cylinder(d=3,h=5,$fn=16);}}\n";
inline const std::string pairDefinition = part + "@{type:Assembly,id:\"pair-def\"} module pair(width=35,gap=60) {"
  "@{type:Component,id:\"left\"} bracket(width=width); translate([gap,0,0]) @{type:Component,id:\"right\"} bracket(width=width+10);}\n";
inline const std::string pair = pairDefinition + "@{type:Component,id:\"pair\"} pair();";
}
