#include "core/provenance/SourceProvenance.h"
#include "core/EvaluationInputs.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/ParserInspection.h"
#include "core/provenance/ProvenanceCollector.h"
#include "core/Builtins.h"
#include "core/BuiltinContext.h"
#include "core/EvaluationSession.h"
#include "core/ScopeContext.h"
#include "core/Tree.h"
#include "core/ModuleInstantiation.h"
#include "core/CSGTreeEvaluator.h"
#include "glview/preview/CSGTreeNormalizer.h"
#include "geometry/GeometryEvaluator.h"
#include "openscad.h"
#include "Feature.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

namespace {
struct ProvenanceFixture {
  std::filesystem::path directory, root;
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceProvenance::SyntaxTable> syntax;
  std::unique_ptr<SourceFileCache> cache;
  std::unique_ptr<SourceFile> parsed;
  std::unique_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> fileContext;
  std::shared_ptr<AbstractNode> treeRoot;
  std::shared_ptr<const SourceProvenance::Result> result;
  ProvenanceFixture() {
    static std::atomic<uint64_t> serial{0};
    directory = std::filesystem::temp_directory_path() /
      ("scad-provenance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "-" + std::to_string(++serial));
    std::filesystem::create_directories(directory / "nested");
    root = directory / "root.scad";
  }
  ~ProvenanceFixture() {
    result.reset(); treeRoot.reset(); fileContext.reset(); builtin.reset(); session.reset(); parsed.reset(); cache.reset();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  SourceInput input(const std::filesystem::path& path, std::string bytes, uint64_t revision = 7) {
    return {EvaluationInputs::fileKey(path), std::move(bytes), {}, "fixture:" + path.string(), revision};
  }
  std::filesystem::path write(const std::string& name, const std::string& bytes) {
    auto path = directory / name;
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
    return path;
  }
  bool parseText(const std::string& bytes, std::vector<SourceInput> editors = {}) {
    write("root.scad", bytes);
    inputs = std::make_shared<EvaluationInputs>(input(root, bytes), std::move(editors));
    syntax = std::make_shared<SourceProvenance::SyntaxTable>(inputs);
    cache = std::make_unique<SourceFileCache>(inputs.get());
    NativeEvaluationScope native(*inputs, *cache);
    SourceProvenance::ParseScope provenance(syntax.get());
    SourceFile *file = nullptr;
    auto success = parse(file, bytes, root.string(), root.string(), false);
    parsed.reset(file);
    if (success) parsed->handleDependencies();
    return success;
  }
  std::vector<const SourceProvenance::Site *> sites(const std::string& name, SourceProvenance::SiteKind kind) {
    std::vector<const SourceProvenance::Site *> result;
    for (const auto& [key, site] : syntax->sites())
      if (site.callable == name && site.kind == kind) result.push_back(&site);
    return result;
  }
  void evaluate() {
    static const bool initialized = [] { Builtins::initialize(); return true; }();
    (void)initialized;
    NativeEvaluationScope native(*inputs, *cache);
    SourceProvenance::ParseScope provenance(syntax.get());
    session = std::make_unique<EvaluationSession>(directory.string());
    session->provenanceCollector = std::make_shared<SourceProvenance::ProvenanceCollector>(syntax);
    auto context = Context::create<BuiltinContext>(session.get());
    builtin = *context;
    AbstractNode::resetIndexCounter();
    treeRoot = parsed->instantiate(builtin, &fileContext);
    REQUIRE(treeRoot);
    REQUIRE(session->provenanceCollector->activeDepth() == 0);
    REQUIRE(SourceProvenance::nativeCollector == nullptr);
    inputs->freeze();
    result = session->provenanceCollector->finish(*treeRoot);
  }
  std::vector<const AbstractNode *> nodes(const std::string& name) {
    std::vector<const AbstractNode *> result;
    std::function<void(const AbstractNode&)> visit = [&](const AbstractNode& node) {
      if (node.name() == name) result.push_back(&node);
      for (const auto& child : node.children) visit(*child);
    };
    visit(*treeRoot);
    return result;
  }
};
using SourceProvenance::SiteKind;
using SourceProvenance::Confidence;
}

TEST_CASE("Provenance exact UTF8 expression and whole spans preserve LF and CRLF", "[source-provenance]") {
  for (const std::string eol : {"\n", "\r\n"}) {
    ProvenanceFixture fixture;
    REQUIRE(fixture.parseText("//\xc3\xa9" + eol + "x=width/2;" + eol + "cube(size = width / 2);"));
    auto x = fixture.sites("x", SiteKind::Assignment);
    REQUIRE(x.size() == 1);
    REQUIRE(fixture.syntax->original(x[0]->whole) == "x=width/2;");
    REQUIRE(x[0]->arguments.size() == 1);
    REQUIRE(x[0]->arguments[0].originalExpression == "width/2");
    if (eol == "\r\n") {
      REQUIRE(x[0]->whole.begin == 6);
      REQUIRE(x[0]->whole.end == 16);
      REQUIRE(x[0]->arguments[0].expression.begin == 8);
      REQUIRE(x[0]->arguments[0].expression.end == 15);
    }
    auto cube = fixture.sites("cube", SiteKind::ModuleCall);
    REQUIRE(cube.size() == 1);
    REQUIRE(fixture.syntax->original(cube[0]->whole) == "cube(size = width / 2);");
    REQUIRE(cube[0]->arguments[0].name == "size");
    REQUIRE(fixture.syntax->original(*cube[0]->arguments[0].nameSpan) == "size");
    REQUIRE(cube[0]->arguments[0].originalExpression == "width / 2");
  }
}

TEST_CASE("Provenance composite strings comments BOM and Unicode keep original bytes", "[source-provenance]") {
  ProvenanceFixture fixture;
  const std::string expression = "\"first\r\n\xc3\xa9 \xf0\x9f\x98\x80 \\\"quote\\\"\"";
  REQUIRE(fixture.parseText("\xef\xbb\xbf/*\xc3\xa9\n*/x=" + expression + ";cube(/*adjacent*/(2 + 3));"));
  auto x = fixture.sites("x", SiteKind::Assignment);
  REQUIRE(x.size() == 1);
  REQUIRE(x[0]->arguments[0].originalExpression == expression);
  auto cube = fixture.sites("cube", SiteKind::ModuleCall);
  REQUIRE(cube.size() == 1);
  REQUIRE(cube[0]->arguments[0].originalExpression == "(2 + 3)");
  REQUIRE(fixture.syntax->original(cube[0]->whole) == "cube(/*adjacent*/(2 + 3));");
}

TEST_CASE("Provenance include and use definitions retain dirty dependency identities", "[source-provenance]") {
  ProvenanceFixture fixture;
  auto included = fixture.write("nested/values.scad", "height=1;");
  auto library = fixture.write("library.scad", "module shape() { cube(1); }");
  REQUIRE(fixture.parseText("include <nested/values.scad>\nuse <library.scad>\nshape();",
    {fixture.input(included, "//\xc3\xa9\r\nheight=11;", 9),
     fixture.input(library, "module shape(size = height / 2) { cube(size); }", 12)}));
  auto height = fixture.sites("height", SiteKind::Assignment);
  REQUIRE(height.size() == 1);
  REQUIRE(height[0]->whole.fileKey == EvaluationInputs::fileKey(included));
  REQUIRE(height[0]->arguments[0].originalExpression == "11");
  auto definition = fixture.sites("shape", SiteKind::ModuleDefinition);
  REQUIRE(definition.size() == 1);
  REQUIRE(definition[0]->whole.fileKey == EvaluationInputs::fileKey(library));
  REQUIRE(definition[0]->arguments[0].originalExpression == "height / 2");
  REQUIRE(definition[0]->head);
  REQUIRE(fixture.syntax->original(*definition[0]->head) == "module shape(size = height / 2)");
  REQUIRE(fixture.syntax->original(definition[0]->whole) == "module shape(size = height / 2) { cube(size); }");
  auto call = fixture.sites("shape", SiteKind::ModuleCall);
  REQUIRE(call.size() == 1);
  REQUIRE(call[0]->whole.fileKey == EvaluationInputs::fileKey(fixture.root));
  REQUIRE(fixture.inputs->read(library).editorRevision == 12);
}

TEST_CASE("Provenance retains reassignment occurrences and complete nested call syntax", "[source-provenance]") {
  ProvenanceFixture fixture;
  REQUIRE(fixture.parseText("x = 1; x = 2; translate([3,0,0]) { cube(x); sphere(2); }"));
  auto assignments = fixture.sites("x", SiteKind::Assignment);
  REQUIRE(assignments.size() == 2);
  REQUIRE(assignments[0]->syntax.ordinal != assignments[1]->syntax.ordinal);
  REQUIRE(assignments[0]->arguments[0].originalExpression == "1");
  REQUIRE(assignments[1]->arguments[0].originalExpression == "2");
  auto transform = fixture.sites("translate", SiteKind::ModuleCall);
  REQUIRE(transform.size() == 1);
  REQUIRE(fixture.syntax->original(transform[0]->whole) == "translate([3,0,0]) { cube(x); sphere(2); }");
  REQUIRE(fixture.parsed->scope->assignments.size() == 1); // Evaluation's effective assignment semantics stay intact.
}

TEST_CASE("Provenance preserves object and fixed key Customizer grammar", "[source-provenance][customizer]") {
  ProvenanceFixture fixture;
  REQUIRE(fixture.parseText("object_value = { a: 1, b: (2 + 3) };\n@{hidden: false, locked: true,} width = 4;\ncube(width);"));
  auto object = fixture.sites("object_value", SiteKind::Assignment);
  REQUIRE(object.size() == 1);
  REQUIRE(object[0]->arguments[0].originalExpression == "{ a: 1, b: (2 + 3) }");
  auto width = fixture.sites("width", SiteKind::Assignment);
  REQUIRE(width.size() == 1);
  REQUIRE(width[0]->arguments[0].originalExpression == "4");
  REQUIRE(fixture.syntax->original(width[0]->whole) == "@{hidden: false, locked: true,} width = 4;");
}

TEST_CASE("Provenance rejects invalid byte boundaries and wrong input fingerprints", "[source-provenance]") {
  ProvenanceFixture fixture;
  REQUIRE(fixture.parseText("//\xc3\xa9\r\nx=2;"));
  auto path = std::make_shared<std::filesystem::path>(fixture.root);
  REQUIRE_FALSE(fixture.syntax->span(Location(1, 4, 1, 5, path))); // Inside é.
  REQUIRE_FALSE(fixture.syntax->span(Location(20, 1, 20, 2, path)));
  auto range = fixture.sites("x", SiteKind::Assignment)[0]->whole;
  range.fingerprint = "different";
  REQUIRE_FALSE(fixture.syntax->original(range));
}

TEST_CASE("Provenance Unicode identifier spans preserve the lexer spelling", "[source-provenance]") {
  struct RestoreFeature {
    bool enabled = Feature::ExperimentalUnicodeIdentifiers.is_enabled();
    RestoreFeature() { Feature::enable_feature("unicode-identifiers", true); }
    ~RestoreFeature() { Feature::enable_feature("unicode-identifiers", enabled); }
  } feature;
  ProvenanceFixture fixture;
  const std::string name = "caf\xc3\xa9";
  REQUIRE(fixture.parseText(name + " = 3; cube(size = " + name + " / 2);"));
  auto assignment = fixture.sites(name, SiteKind::Assignment);
  REQUIRE(assignment.size() == 1);
  REQUIRE(fixture.syntax->original(*assignment[0]->arguments[0].nameSpan) == name);
  auto cube = fixture.sites("cube", SiteKind::ModuleCall);
  REQUIRE(cube.size() == 1);
  REQUIRE(cube[0]->arguments[0].originalExpression == name + " / 2");
}

TEST_CASE("Provenance native parsing remains independent of HTTP inspection token limits", "[source-provenance]") {
  ProvenanceFixture fixture;
  std::string text;
  for (unsigned i = 0; i < 3000; ++i) text += "x" + std::to_string(i) + "=1;\n";
  text += "cube(1);";
  REQUIRE(fixture.parseText(text));
  REQUIRE(fixture.syntax->sites().size() == 3001);
  REQUIRE(fixture.parsed->scope->assignments.size() == 3000);
  REQUIRE(SourceProvenance::nativeSyntaxTable == nullptr);
  REQUIRE(nativeEvaluationInputs == nullptr);
}

TEST_CASE("Provenance does not fabricate a whole call across include file boundaries", "[source-provenance]") {
  ProvenanceFixture fixture;
  auto child = fixture.write("nested/shape.scad", "cube(2);");
  REQUIRE(fixture.parseText("translate([1,0,0]) include <nested/shape.scad>"));
  auto wrapper = fixture.sites("translate", SiteKind::ModuleCall);
  REQUIRE(wrapper.size() == 1);
  REQUIRE_FALSE(wrapper[0]->wholeComplete);
  REQUIRE_FALSE(fixture.syntax->original(wrapper[0]->whole));
  REQUIRE(wrapper[0]->head);
  REQUIRE(fixture.syntax->original(*wrapper[0]->head) == "translate([1,0,0])");
  REQUIRE(wrapper[0]->arguments[0].originalExpression == "[1,0,0]");
  auto cube = fixture.sites("cube", SiteKind::ModuleCall);
  REQUIRE(cube.size() == 1);
  REQUIRE(cube[0]->wholeComplete);
  REQUIRE(cube[0]->whole.fileKey == EvaluationInputs::fileKey(child));
  REQUIRE(fixture.syntax->original(cube[0]->whole) == "cube(2);");
}

TEST_CASE("Provenance repeated modules keep call definition and invocation paths separate", "[source-provenance]") {
  ProvenanceFixture fixture;
  auto library = fixture.write("library.scad", "module part() { cube(2); }");
  REQUIRE(fixture.parseText("use <library.scad>\ntranslate([-8,0,0]) part(); translate([8,0,0]) part();"));
  fixture.evaluate();
  auto cubes = fixture.nodes("cube");
  REQUIRE(cubes.size() == 2);
  auto a = fixture.result->forNode(cubes[0]->index()), b = fixture.result->forNode(cubes[1]->index());
  REQUIRE(a.confidence == Confidence::Unique);
  REQUIRE(b.confidence == Confidence::Unique);
  REQUIRE(a.candidateInstances != b.candidateInstances);
  auto first = fixture.result->instance(a.candidateInstances.front());
  auto second = fixture.result->instance(b.candidateInstances.front());
  REQUIRE(first->call.syntax == second->call.syntax); // One definition-body call, evaluated twice.
  REQUIRE(first->parent != second->parent);
  auto firstPart = fixture.result->instance(*first->parent), secondPart = fixture.result->instance(*second->parent);
  REQUIRE(firstPart->definition);
  REQUIRE(secondPart->definition);
  REQUIRE(firstPart->definition->syntax == secondPart->definition->syntax);
  REQUIRE_FALSE(firstPart->call.syntax == secondPart->call.syntax);
  REQUIRE(firstPart->definition->whole.fileKey == EvaluationInputs::fileKey(library));
  REQUIRE(firstPart->call.whole.fileKey == EvaluationInputs::fileKey(fixture.root));
  REQUIRE(first->path.size() == 3);
  REQUIRE(a.localToWorld->translation().isApprox(Vector3d(-8,0,0)));
  REQUIRE(b.localToWorld->translation().isApprox(Vector3d(8,0,0)));
  REQUIRE(fixture.result->finalGeometry().confidence == Confidence::Multiple);
}

TEST_CASE("Provenance loops and children retain distinct emitting invocations", "[source-provenance]") {
  ProvenanceFixture fixture;
  REQUIRE(fixture.parseText("module wrap() { children(); } for(x=[-8,0,8]) translate([x,0,0]) wrap() cube(2);"));
  fixture.evaluate();
  auto cubes = fixture.nodes("cube");
  REQUIRE(cubes.size() == 3);
  std::set<uint64_t> identities;
  for (size_t i = 0; i < cubes.size(); ++i) {
    auto map = fixture.result->forNode(cubes[i]->index());
    REQUIRE(map.confidence == Confidence::Unique);
    identities.insert(map.candidateInstances.front());
    REQUIRE(map.localToWorld->translation().isApprox(Vector3d(8.0*i-8,0,0)));
    auto instance = fixture.result->instance(map.candidateInstances.front());
    REQUIRE(instance->path.size() == 5);
  }
  REQUIRE(identities.size() == 3);
}

TEST_CASE("Provenance cached geometry preserves CSG instance transforms through normalization", "[source-provenance]") {
  ProvenanceFixture fixture;
  REQUIRE(fixture.parseText("for(x=[-8,8]) translate([x,0,0]) mirror([1,0,0]) scale([2,1,0.5]) cube(2);"));
  fixture.evaluate();
  auto cubes = fixture.nodes("cube");
  REQUIRE(cubes.size() == 2);
  Tree tree(fixture.treeRoot, fixture.directory.string());
  GeometryEvaluator geometry(tree);
  auto first = geometry.evaluateGeometry(*cubes[0], false), second = geometry.evaluateGeometry(*cubes[1], false);
  REQUIRE(first);
  REQUIRE(first == second); // Actual shared Geometry cache hit.
  CSGTreeEvaluator csg(tree, &geometry);
  auto raw = csg.buildCSGTree(*fixture.treeRoot);
  auto result = fixture.result->withPreview({raw});
  CSGTreeNormalizer normalizer(1000);
  CSGProducts products;
  products.import(normalizer.normalize(raw));
  size_t leaves = 0;
  for (const auto& product : products.products) for (const auto& object : product.intersections) {
    auto mapping = result->forNode(object.leaf->index);
    REQUIRE(mapping.confidence == Confidence::Unique);
    REQUIRE(mapping.localToWorld->matrix().isApprox(object.leaf->matrix.matrix(), 1e-12));
    REQUIRE(mapping.localToWorld->linear().isApprox(Vector3d(-2,1,0.5).asDiagonal().toDenseMatrix()));
    REQUIRE(mapping.invertible);
    ++leaves;
  }
  REQUIRE(leaves == 2);
  REQUIRE(result->forNode(cubes[0]->index()).candidateInstances != result->forNode(cubes[1]->index()).candidateInstances);
}

TEST_CASE("Provenance singular frames and subtraction boundaries are explicit", "[source-provenance]") {
  ProvenanceFixture fixture;
  REQUIRE(fixture.parseText("difference(){cube(10); translate([2,0,0]) cube(3);} scale([0,1,1]) sphere(2);"));
  fixture.evaluate();
  auto cubes = fixture.nodes("cube");
  REQUIRE(cubes.size() == 2);
  REQUIRE(fixture.result->forNode(cubes[0]->index()).boundaryPolarity == 1);
  REQUIRE(fixture.result->forNode(cubes[1]->index()).boundaryPolarity == -1);
  auto singular = fixture.result->forNode(fixture.nodes("sphere")[0]->index());
  REQUIRE(singular.localToWorld);
  REQUIRE_FALSE(singular.invertible);
  REQUIRE(fixture.result->finalGeometry().confidence == Confidence::Multiple);
  REQUIRE_FALSE(fixture.result->finalGeometry().localToWorld);
}

TEST_CASE("Provenance derived geometry identifies its producing call without inner face claims", "[source-provenance]") {
  for (const std::string operation : {"render", "hull"}) {
    ProvenanceFixture fixture;
    REQUIRE(fixture.parseText(operation + "(){cube(2); translate([3,0,0]) cube(2);}"));
    fixture.evaluate();
    const auto mapping = fixture.result->finalGeometry();
    REQUIRE(mapping.confidence == Confidence::Unique);
    REQUIRE(mapping.candidateInstances.size() == 1);
    REQUIRE(fixture.result->instance(mapping.candidateInstances[0])->call.callable == operation);
    REQUIRE(mapping.reason.find("no per-face") != std::string::npos);
  }
}

TEST_CASE("Provenance evaluation failures unwind frames and publication outlives ASTs", "[source-provenance]") {
  std::shared_ptr<const SourceProvenance::Result> retained;
  {
    ProvenanceFixture fixture;
    REQUIRE(fixture.parseText("module fail(){ assert(false, \"fixture\"); } fail();"));
    fixture.evaluate();
    retained = fixture.result;
    REQUIRE(fixture.session->provenanceCollector->activeDepth() == 0);
    REQUIRE(SourceProvenance::nativeCollector == nullptr);
  }
  REQUIRE(retained);
  REQUIRE_FALSE(retained->instances().empty());
  for (const auto& [id, instance] : retained->instances()) {
    REQUIRE(retained->syntaxTable().original(instance.call.whole));
  }
}
