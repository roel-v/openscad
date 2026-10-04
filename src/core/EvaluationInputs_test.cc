#include "core/EvaluationInputs.h"
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
