#include "core/EvaluationInputs.h"
#include "core/EvaluationSession.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/node.h"
#include "core/Context.h"
#include "core/parsersettings.h"
#include "openscad.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <chrono>

namespace {
void guarded(const std::string& text, std::vector<NativeCapability>& calls)
{
  static const bool initialized=[] { Builtins::initialize(); return true; }();(void)initialized;
  auto root=std::filesystem::temp_directory_path()/"native-capability-fixture.scad";
  EvaluationInputs inputs(SourceInput{EvaluationInputs::fileKey(root),text,{},{},{}}, {}, EvaluationInputs::ReadPolicy::RejectMissing);
  SourceFileCache cache(&inputs);NativeEvaluationScope native(inputs,cache);
  SourceFile *raw=nullptr;REQUIRE(parse(raw,text,root.string(),root.string(),false));std::unique_ptr<SourceFile> parsed(raw);
  EvaluationSession session(root.parent_path().string());
  session.capabilityGuard=[&](NativeCapability capability){calls.push_back(capability);throw EvaluationCapabilityError("Captured-input-only evaluation rejects capability");};
  auto context=Context::create<BuiltinContext>(&session);std::shared_ptr<const FileContext> file;
  auto node=parsed->instantiate(*context,&file);REQUIRE(node);
}
}

TEST_CASE("Native capability guard rejects actual external module calls including indirect user calls", "[sealed-evaluation-inputs][native-capabilities]")
{
  for(const auto& text : {"import(\"missing.stl\");", "surface(\"missing.dat\");", "text(\"a\");", "module indirect(){surface(\"missing.dat\");} indirect();"}) {
    std::vector<NativeCapability> calls;REQUIRE_THROWS_AS(guarded(text,calls),EvaluationCapabilityError);REQUIRE(calls.size()==1);
  }
}
TEST_CASE("Native capability guard permits unused external library declarations and shadowed builtin spellings", "[sealed-evaluation-inputs][native-capabilities]")
{
  std::vector<NativeCapability> calls;
  guarded("module unused(){import(\"missing.stl\");text(\"a\");} module import(){cube(1);} import();",calls);
  REQUIRE(calls.empty());
}
TEST_CASE("Native capability guard rejects indirect unseeded randomness and permits explicit seeded randomness", "[sealed-evaluation-inputs][native-capabilities]")
{
  std::vector<NativeCapability> calls;
  REQUIRE_THROWS_AS(guarded("function indirect()=rands(1,2,1);cube(indirect()[0]);",calls),EvaluationCapabilityError);
  REQUIRE(calls==std::vector<NativeCapability>{NativeCapability::UnseededRandom});calls.clear();
  guarded("cube(rands(1,2,1,123)[0]);",calls);REQUIRE(calls.empty());
}
TEST_CASE("Explicit native library configuration retains exact order without ambient additions", "[sealed-evaluation-inputs][native-capabilities]")
{
  NativeExecutionGuard lock;const auto original=get_library_path();
  struct Restore { std::vector<std::string> paths;~Restore(){set_library_path(std::move(paths));} } restore{original};
  const std::vector<std::string> explicitPaths={"C:/first", "C:/second", "C:/first"};
  set_library_path(explicitPaths);REQUIRE(get_library_path()==explicitPaths);
  set_library_path({});REQUIRE(get_library_path().empty());
}
