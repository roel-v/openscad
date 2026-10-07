#include "core/EvaluationInputs.h"
#include "core/EvaluationSession.h"
#include "core/SourceFileCache.h"
#include "core/SourceFile.h"
#include "core/UserModule.h"
#include "openscad.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <glib.h>

namespace {
struct InputFixture {
  std::filesystem::path directory;
  InputFixture() {
    auto id = g_uuid_string_random();
    directory = std::filesystem::temp_directory_path() / (std::string("scad-evaluation-") + id);
    g_free(id);
    std::filesystem::create_directories(directory / "nested");
  }
  ~InputFixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
  std::filesystem::path write(const std::string& name, const std::string& bytes) {
    auto path = directory / name;
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("Input fixture write failed");
    return path;
  }
  SourceInput input(const std::filesystem::path& path, std::string bytes, std::string editor = "editor", uint64_t revision = 7) {
    return {EvaluationInputs::fileKey(path), std::move(bytes), {}, std::move(editor), revision};
  }
};
}

TEST_CASE("Evaluation inputs capture dirty editors and retain exact UTF8 CRLF BOM bytes", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto root = fixture.write("root.scad", "cube(1);\n");
  const auto include = fixture.write("nested/include.scad", "height=1;\n");
  const std::string exact = "\xef\xbb\xbf//\xc3\xa9\r\nheight=7;\r\n";
  EvaluationInputs inputs(fixture.input(root, "include <nested/include.scad>;"), {fixture.input(include, exact)});
  const auto& captured = inputs.read(include);
  REQUIRE(captured.utf8 == exact);
  REQUIRE(captured.fingerprint == EvaluationInputs::fingerprint(exact));
  REQUIRE(captured.editorRevision == 7);
  REQUIRE(inputs.consumed().size() == 2);
  fixture.write("nested/include.scad", "height=9;\n");
  REQUIRE(inputs.read(include).utf8 == exact);
}

TEST_CASE("Input observation follows consumed dependencies and ends when frozen", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto root = fixture.write("root.scad", "cube(1);");
  const auto dependency = fixture.write("dependency.scad", "height=1;");
  EvaluationInputs inputs(fixture.input(root, "cube(1);"), {fixture.input(dependency, "height=2;", "dependency")});
  std::vector<std::string> observed;
  inputs.observeReads([&](const SourceInput& input) { observed.push_back(input.editorId.value_or("disk")); });
  REQUIRE(observed.empty());
  REQUIRE(inputs.read(dependency).utf8 == "height=2;");
  inputs.read(dependency);
  REQUIRE(observed == std::vector<std::string>{"dependency", "dependency"});
  inputs.freeze();
  inputs.read(dependency);
  REQUIRE(observed.size() == 2);
}

TEST_CASE("Unresolved library lookups do not consume captured inputs", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto root = fixture.write("root.scad", "use <>\ncube(1);");
  EvaluationInputs inputs(fixture.input(root, "use <>\ncube(1);"));
  SourceFileCache cache(&inputs);
  const auto consumed = inputs.consumed().size();
  REQUIRE(cache.lookup("") == nullptr);
  inputs.freeze();
  REQUIRE(cache.lookup("") == nullptr);
  REQUIRE(inputs.consumed().size() == consumed);
}

TEST_CASE("Library lookup aliases stay within their captured evaluation", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto root = fixture.write("root.scad", "cube(1);");
  const auto library = fixture.write("library.scad", "module old_shape() { cube(1); }");
  EvaluationInputs first(fixture.input(root, "cube(1);"));
  SourceFileCache firstCache(&first);
  SourceFile *oldFile = nullptr;
  firstCache.process(root.string(), library.string(), oldFile);
  REQUIRE(oldFile);
  const auto alias = (fixture.directory / "nested/../library.scad").string();
  REQUIRE(firstCache.lookup(alias) == oldFile);
  fixture.write("library.scad", "module new_shape() { sphere(1); }");
  REQUIRE(firstCache.lookup(alias) == oldFile);
  EvaluationInputs second(fixture.input(root, "cube(1);"));
  SourceFileCache secondCache(&second);
  SourceFile *newFile = nullptr;
  secondCache.process(root.string(), library.string(), newFile);
  REQUIRE(newFile);
  REQUIRE(newFile != oldFile);
  REQUIRE(secondCache.lookup(alias) == newFile);
  REQUIRE(second.consumed().at(EvaluationInputs::fileKey(library)).utf8 !=
          first.consumed().at(EvaluationInputs::fileKey(library)).utf8);
}

TEST_CASE("Captured library aliases respect the current directory for relative lookups", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto root = fixture.write("root.scad", "cube(1);");
  const auto first = fixture.write("library.scad", "module first() {} ");
  const auto second = fixture.write("nested/library.scad", "module second() {} ");
  EvaluationInputs inputs(fixture.input(root, "cube(1);"));
  SourceFileCache cache(&inputs);
  SourceFile *firstFile = nullptr, *secondFile = nullptr;
  cache.process(root.string(), first.string(), firstFile);
  cache.process(root.string(), second.string(), secondFile);
  REQUIRE(firstFile);
  REQUIRE(secondFile);
  REQUIRE(firstFile != secondFile);
  struct RestoreDirectory {
    std::filesystem::path saved = std::filesystem::current_path();
    ~RestoreDirectory() { std::error_code error; std::filesystem::current_path(saved, error); }
  } restore;
  std::filesystem::current_path(fixture.directory);
  REQUIRE(cache.lookup("library.scad") == firstFile);
  REQUIRE(cache.lookup(first.string()) == firstFile);
  REQUIRE(cache.lookup(first.string()) == firstFile);
  std::filesystem::current_path(fixture.directory / "nested");
  REQUIRE(cache.lookup("library.scad") == secondFile);
  REQUIRE(cache.lookup(first.string()) == firstFile);
}

TEST_CASE("Evaluation inputs read each disk dependency once and freeze consumed sources", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto root = fixture.write("root.scad", "cube(1);");
  const auto dependency = fixture.write("dependency.scad", "x=1;\r\n");
  EvaluationInputs inputs(fixture.input(root, "cube(1);"));
  REQUIRE(inputs.read(dependency).utf8 == "x=1;\r\n");
  fixture.write("dependency.scad", "x=2;\n");
  REQUIRE(inputs.read(fixture.directory / "nested/../dependency.scad").utf8 == "x=1;\r\n");
  inputs.freeze();
  REQUIRE(inputs.read(dependency).utf8 == "x=1;\r\n");
  REQUIRE_THROWS_AS(inputs.read(fixture.write("other.scad", "cube(2);")), EvaluationInputError);
}

TEST_CASE("Evaluation inputs reject conflicting canonical buffers and share identical overrides", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto root = fixture.write("root.scad", "cube(1);");
  auto first = fixture.input(root, "cube(2);", "one");
  auto same = fixture.input(fixture.directory / "nested/../root.scad", "cube(2);", "two");
  EvaluationInputs inputs(first, {first, same});
  REQUIRE(inputs.consumed().size() == 1);
  REQUIRE(inputs.capturedEditors().size() == 2);
  same.utf8 = "cube(3);";
  REQUIRE_THROWS_AS(EvaluationInputs(first, {first, same}), EvaluationInputError);
}

TEST_CASE("Evaluation inputs reject invalid UTF8 and NUL without changing bytes", "[evaluation-inputs]")
{
  for (const auto& bytes : {std::string("x\0y", 3), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80")})
    REQUIRE_THROWS_AS(EvaluationInputs(SourceInput{"session:test", bytes, {}, {}, {}}), EvaluationInputError);
  EvaluationInputs empty(SourceInput{"session:untitled", "", {}, "untitled", 0});
  REQUIRE(empty.root().utf8.empty());
}

TEST_CASE("Native evaluation reader and library cache scopes restore after exceptions", "[evaluation-inputs]")
{
  EvaluationInputs inputs(SourceInput{"session:test", "cube(1);", {}, {}, {}});
  SourceFileCache cache(&inputs);
  auto original = SourceFileCache::instance();
  REQUIRE(nativeEvaluationInputs == nullptr);
  try {
    NativeEvaluationScope scope(inputs, cache);
    REQUIRE(nativeEvaluationInputs == &inputs);
    REQUIRE(SourceFileCache::instance() == &cache);
    throw std::runtime_error("fixture");
  } catch (const std::runtime_error&) {}
  REQUIRE(nativeEvaluationInputs == nullptr);
  REQUIRE(SourceFileCache::instance() == original);
}

TEST_CASE("Native parser resource capability guards restore after rejected font imports", "[sealed-evaluation-inputs][evaluation-inputs]")
{
  InputFixture fixture;
  fixture.write("font.ttf", "invalid bytes must not reach the font loader");
  const auto path=fixture.write("root.scad", "use <font.ttf>\ncube(1);");
  EvaluationInputs inputs(fixture.input(path,EvaluationInputs::diskBytes(path)));
  SourceFileCache cache(&inputs);
  REQUIRE_FALSE(nativeParserCapabilityGuard);
  unsigned requests=0;
  {
    NativeEvaluationScope guarded(inputs,cache,[&](NativeCapability capability){
      REQUIRE(capability==NativeCapability::FontInput);++requests;
      throw EvaluationCapabilityError("font fixture rejected before IO");
    });
    {
      NativeEvaluationScope nested(inputs,cache);
      REQUIRE_FALSE(nativeParserCapabilityGuard);
    }
    REQUIRE(nativeParserCapabilityGuard);
    SourceFile *raw=nullptr;
    CHECK_THROWS_AS(parse(raw,inputs.root().utf8,path.string(),path.string(),false),EvaluationCapabilityError);
    std::unique_ptr<SourceFile> partial(raw);
    CHECK(requests==1);
    raw=nullptr;
    REQUIRE(parse(raw,"cube(2);",path.string(),path.string(),false));
    std::unique_ptr<SourceFile> recovered(raw);
  }
  REQUIRE_FALSE(nativeParserCapabilityGuard);
  REQUIRE(nativeEvaluationInputs==nullptr);
}

TEST_CASE("Native include and use parsers consume one captured input request", "[evaluation-inputs]")
{
  InputFixture fixture;
  const auto include = fixture.write("nested/values.scad", "height=1;");
  const auto library = fixture.write("helper.scad", "function size_value()=1;");
  const auto root = fixture.write("root.scad", "include <nested/values.scad>\nuse <helper.scad>\ncube(height+size_value());");
  EvaluationInputs inputs(fixture.input(root, EvaluationInputs::diskBytes(root)),
    {fixture.input(include, "//\xc3\xa9\r\nheight=7;\r\n"), fixture.input(library, "function size_value()=11;")});
  SourceFileCache cache(&inputs);
  NativeEvaluationScope scope(inputs, cache);
  SourceFile *parsed = nullptr;
  REQUIRE(parse(parsed, inputs.root().utf8, root.string(), root.string(), false));
  std::unique_ptr<SourceFile> owner(parsed);
  owner->handleDependencies();
  REQUIRE(inputs.consumed().size() == 3);
  REQUIRE(cache.lookup(library.string()) != nullptr);
  REQUIRE(inputs.read(include).utf8 == "//\xc3\xa9\r\nheight=7;\r\n");
  REQUIRE(inputs.read(library).utf8 == "function size_value()=11;");
  REQUIRE(cache.size() == 1);
}

TEST_CASE("Captured AST cache seeding owns native declarations and ignores later source files", "[evaluation-inputs]") {
  InputFixture fixture; const auto root = fixture.write("root.scad", "module retained(size=2){cube(size);}");
  EvaluationInputs inputs(fixture.input(root, EvaluationInputs::diskBytes(root)));
  SourceFileCache original(&inputs); std::unique_ptr<SourceFile> parsed;
  { NativeEvaluationScope native(inputs, original); SourceFile *file=nullptr;
    REQUIRE(parse(file, inputs.root().utf8, root.string(), root.string(), false)); parsed.reset(file); }
  inputs.freeze(); SourceFileCache seeded(&inputs); seeded.seedCaptured(inputs.root().fileKey, *parsed);
  const auto scope = parsed->scope; parsed.reset(); fixture.write("root.scad", "module replacement(){}");
  REQUIRE(seeded.lookup(root.string())); REQUIRE(seeded.lookup(root.string())->scope == scope);
  REQUIRE(seeded.lookup(root.string())->scope->lookup<UserModule *>("retained"));
  REQUIRE_THROWS_AS(seeded.seedCaptured(inputs.root().fileKey, *seeded.lookup(root.string())), EvaluationInputError);
  SourceFile missing(root.parent_path().string(), "missing.scad"); REQUIRE_THROWS_AS(seeded.seedCaptured("uncaptured", missing), EvaluationInputError);
  SourceFileCache global; REQUIRE_THROWS_AS(global.seedCaptured(inputs.root().fileKey, *seeded.lookup(root.string())), EvaluationInputError);
}

TEST_CASE("Sealed inputs retain dirty nested sources and never read missing disk bytes", "[sealed-evaluation-inputs][evaluation-inputs]")
{
  InputFixture fixture;auto root=fixture.write("root.scad","cube(1);");
  auto include=fixture.write("nested/include.scad","disk bytes must not win");
  const auto dirty=fixture.input(include,"height=7;\r\n");
  EvaluationInputs inputs(fixture.input(root,"cube(1);"),{dirty},EvaluationInputs::ReadPolicy::RejectMissing,
    {{dirty.fileKey,dirty}});
  REQUIRE(inputs.read(include).utf8==dirty.utf8);
  auto unseeded=fixture.write("unseeded.scad",std::string("\xc0\xaf"));
  try {inputs.read(unseeded);FAIL("Missing seed must not read invalid disk bytes");}
  catch(const MissingCapturedInput& error){REQUIRE(error.key==EvaluationInputs::fileKey(unseeded));}
  REQUIRE(inputs.consumed().size()==2);
}

TEST_CASE("Sealed seed identity and fingerprint conflicts fail without choosing a version", "[sealed-evaluation-inputs]")
{
  InputFixture fixture;auto root=fixture.write("root.scad","cube(1);");auto input=fixture.input(root,"cube(1);");
  auto conflicting=input;conflicting.utf8="cube(2);";
  REQUIRE_THROWS_AS(EvaluationInputs(input,{},EvaluationInputs::ReadPolicy::RejectMissing,{{input.fileKey,conflicting}}),EvaluationInputError);
  auto badHash=input;badHash.fingerprint=std::string(64,'0');
  REQUIRE_THROWS_AS(EvaluationInputs(badHash,{},EvaluationInputs::ReadPolicy::RejectMissing),EvaluationInputError);
  REQUIRE_THROWS_AS(EvaluationInputs(input,{},EvaluationInputs::ReadPolicy::RejectMissing,{{"wrong key",input}}),EvaluationInputError);
  auto same=input;same.editorId="second";
  EvaluationInputs equal(input,{input,same},EvaluationInputs::ReadPolicy::RejectMissing,{{input.fileKey,input}});
  REQUIRE(equal.capturedEditors().size()==2);
}

TEST_CASE("Hypothetical replacement preserves exact base currentness rather than fabricating an editor revision", "[sealed-evaluation-inputs]")
{
  InputFixture fixture;const std::string bytes="\xef\xbb\xbf//\xc3\xa9\r\ncube(1);\r\n";
  auto root=fixture.write("root.scad",bytes);auto original=fixture.input(root,bytes,"owner",19);
  EvaluationInputs base(original,{original},EvaluationInputs::ReadPolicy::RejectMissing);base.freeze();
  const std::string candidate="\xef\xbb\xbf//\xc3\xa9\r\ncube(2);\r\n";
  auto replaced=base.withReplacement(original.fileKey,candidate);
  REQUIRE(base.root().utf8==bytes);REQUIRE(replaced.root().utf8==candidate);
  REQUIRE(replaced.root().fingerprint==EvaluationInputs::fingerprint(candidate));
  REQUIRE_FALSE(replaced.root().editorId);REQUIRE_FALSE(replaced.root().editorRevision);
  REQUIRE(replaced.validationInputs().at(original.fileKey).utf8==bytes);
  REQUIRE(replaced.validationInputs().at(original.fileKey).editorRevision==19);
  REQUIRE(replaced.capturedEditors().front().editorRevision==19);
  REQUIRE_THROWS_AS(replaced.withReplacement(original.fileKey,bytes),EvaluationInputError);
  REQUIRE_THROWS_AS(base.withReplacement("session:uncaptured",candidate),MissingCapturedInput);
}
TEST_CASE("Sealed native include and use report missing canonical inputs and recover lexer state without disk bytes", "[sealed-evaluation-inputs]")
{
  for (bool include : {true,false}) {
    InputFixture fixture;
    auto dependency=fixture.write("nested/source.scad",std::string("\xc0\xaf"));
    const std::string text=include?"include <nested/source.scad>\ncube(height);":"use <nested/source.scad>\ncube(height());";
    auto root=fixture.write("root.scad",text);const auto rootInput=fixture.input(root,text);
    EvaluationInputs missing(rootInput,{},EvaluationInputs::ReadPolicy::RejectMissing);SourceFileCache missingCache(&missing);
    SourceFile *raw=nullptr;bool requested=false;
    {
      NativeEvaluationScope native(missing,missingCache);
      try {const bool parsed=parse(raw,text,root.string(),root.string(),false);REQUIRE(parsed);raw->handleDependencies();}
      catch(const MissingCapturedInput& error){requested=true;REQUIRE(error.key==EvaluationInputs::fileKey(dependency));}
    }
    std::unique_ptr<SourceFile> partial(raw);REQUIRE(requested);REQUIRE(missing.consumed().size()==1);
    const auto seed=fixture.input(dependency,include?"height=27;":"function height()=27;");
    EvaluationInputs seeded(rootInput,{},EvaluationInputs::ReadPolicy::RejectMissing,{{seed.fileKey,seed}});SourceFileCache seededCache(&seeded);
    NativeEvaluationScope native(seeded,seededCache);SourceFile *recovered=nullptr;
    REQUIRE(parse(recovered,text,root.string(),root.string(),false));std::unique_ptr<SourceFile> owner(recovered);owner->handleDependencies();
    REQUIRE(seeded.consumed().size()==2);REQUIRE(seeded.read(dependency).utf8==seed.utf8);
  }
}
