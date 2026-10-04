#include "core/SurfaceNode.h"

#include <algorithm>
#include <boost/property_tree/ptree.hpp>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/BuiltinContext.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/Expression.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/customizer/ParameterObject.h"
#include "openscad.h"

namespace fs = std::filesystem;

static std::string get_test_image(const std::string& name)
{
  return std::string(OPENSCAD_TEST_DATA_DIR) + "/image/" + name;
}

namespace {

class TemporaryCustomizerDirectory
{
public:
  TemporaryCustomizerDirectory()
  {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() / ("openscad-customizer-tests-" + std::to_string(unique));
    fs::create_directories(path_);
  }

  ~TemporaryCustomizerDirectory()
  {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const { return path_; }

  void writeFile(const std::string& name, const std::string& contents) const
  {
    const fs::path filename = path_ / name;
    std::ofstream output(filename);
    if (!output) {
      throw std::runtime_error("Could not create Customizer test fixture: " + filename.string());
    }
    output << contents;
    if (!output) {
      throw std::runtime_error("Could not write Customizer test fixture: " + filename.string());
    }
  }

private:
  fs::path path_;
};

template <typename Callback>
void withParsedParameters(const std::string& source, const fs::path& filename, Callback&& callback)
{
  SourceFile *parsedSourceFile = nullptr;
  INFO("Parsing " << filename.string());
  REQUIRE(parse(parsedSourceFile, source, filename.string(), filename.string(), false));
  REQUIRE(parsedSourceFile != nullptr);
  std::unique_ptr<SourceFile> sourceFile(parsedSourceFile);

  EvaluationSession session(filename.parent_path().string());
  auto builtinContext = Context::create<BuiltinContext>(&session);
  auto fileContext = Context::create<FileContext>(*builtinContext, sourceFile.get());

  ParameterObjects parameters =
    ParameterObjects::fromSourceFile(sourceFile.get(), fileContext.operator->());
  callback(parameters, fileContext.operator->());
}

ParameterObject& parameterNamed(ParameterObjects& parameters, const std::string& name)
{
  const auto parameter =
    std::find_if(parameters.begin(), parameters.end(), [&name](const auto& candidate) {
      return candidate->name() == name;
    });
  if (parameter == parameters.end()) {
    throw std::runtime_error("Customizer parameter not found: " + name);
  }
  return **parameter;
}

void updateParameterStates(ParameterObjects& parameters, Context *context)
{
  for (const auto& parameter : parameters) {
    parameter->updateContext(context);
  }
  for (const auto& parameter : parameters) {
    parameter->updateAttributes(context);
  }
}

void setStringValue(ParameterObject& parameter, const std::string& value)
{
  boost::property_tree::ptree encodedValue;
  encodedValue.put_value(value);
  REQUIRE(parameter.importValue(std::move(encodedValue), true));
}

const std::string selectorAttributes = R"(
@{
  range: [
    ["hide", "Hide"],
    ["show", "Show"]
  ]
}
)";

}  // namespace

TEST_CASE("Native attributes use fixed OEP8-style members", "[customizer][parameter-attributes][parser]")
{
  const auto source = GENERATE(
    "@{} size = 10;",
    "@{hidden: false} size = 10;",
    "@{hidden: false,} size = 10;",
    "@{range: [1:2:9], hidden: true ? false : true} size = 5;");
  TemporaryCustomizerDirectory files;
  const auto filename = (files.path() / "attribute_syntax.scad").string();
  SourceFile *parsed = nullptr;
  REQUIRE(parse(parsed, source, filename, filename, false));
  std::unique_ptr<SourceFile> owner(parsed);
  REQUIRE(parsed != nullptr);
  REQUIRE(parsed->scope->assignments.size() == 1);
  const auto *annotation = parsed->scope->assignments.front()->annotation("NativeAttributes");
  REQUIRE(annotation != nullptr);
  std::ostringstream printed;
  annotation->getExpr()->print(printed, "");
  SourceFile *reparsed = nullptr;
  const auto roundTrip = printed.str() + " size = 10;";
  REQUIRE(parse(reparsed, roundTrip, filename, filename, false));
  std::unique_ptr<SourceFile> roundTripOwner(reparsed);
}

TEST_CASE("Native attributes reject old syntax and dynamic object construction",
          "[customizer][parameter-attributes][parser]")
{
  const auto source = GENERATE(
    "@{hidden = false;} size = 10;",
    "@{hidden: false;} size = 10;",
    "@{hidden: false locked: true} size = 10;",
    "@{,} size = 10;",
    "@{\"hidden\": false} size = 10;",
    "@{(\"hidden\"): false} size = 10;",
    "@{each object(hidden = false)} size = 10;",
    "@{for (i = [1:2]) (str(i)): false} size = 10;",
    "@{if (true) hidden: false} size = 10;");
  TemporaryCustomizerDirectory files;
  const auto filename = (files.path() / "invalid_attribute_syntax.scad").string();
  SourceFile *parsed = nullptr;
  CHECK_FALSE(parse(parsed, source, filename, filename, false));
  std::unique_ptr<SourceFile> owner(parsed);
}

TEST_CASE("Native Customizer attributes configure number parameters",
          "[customizer][parameter-attributes]")
{
  const std::string source = R"(
@{
  description: "Cube size",
  group: "Geometry",
  min: 1,
  max: 100,
  step: 0.5,
  slider: false,
  hidden: false,
  locked: true,
}
cube_size = 36;
)";

  TemporaryCustomizerDirectory files;
  withParsedParameters(
    source, files.path() / "inline_native_attributes.scad",
    [](ParameterObjects& parameters, Context *context) {
      REQUIRE(parameters.size() == 1);
      auto& cubeSize = parameterNamed(parameters, "cube_size");
      auto *number = dynamic_cast<NumberParameter *>(&cubeSize);
      REQUIRE(number != nullptr);

      updateParameterStates(parameters, context);

      CHECK(cubeSize.group() == "Geometry");
      CHECK(cubeSize.description() == " Cube size");
      CHECK_FALSE(cubeSize.isHidden());
      CHECK(cubeSize.isLocked());
      REQUIRE(number->minimum);
      REQUIRE(number->maximum);
      REQUIRE(number->step);
      CHECK(*number->minimum == 1);
      CHECK(*number->maximum == 100);
      CHECK(*number->step == 0.5);
      CHECK_FALSE(number->isSliderEnabled());
    });
}

TEST_CASE("Ordinary parameters dynamically control hidden and locked attributes",
          "[customizer][parameter-attributes][dynamic]")
{
  const std::string source = selectorAttributes + R"(
selector = "hide";

@{
  hidden: selector != "show",
  locked: selector == "hide"
}
cube_size = 36;
)";

  TemporaryCustomizerDirectory files;
  withParsedParameters(
    source, files.path() / "inline_dynamic_attributes.scad",
    [](ParameterObjects& parameters, Context *context) {
      auto& selector = parameterNamed(parameters, "selector");
      auto& cubeSize = parameterNamed(parameters, "cube_size");

      CHECK(cubeSize.getDependencies() == std::set<std::string>{"selector"});

      updateParameterStates(parameters, context);
      CHECK(cubeSize.isHidden());
      CHECK(cubeSize.isLocked());

      setStringValue(selector, "show");
      updateParameterStates(parameters, context);
      CHECK_FALSE(cubeSize.isHidden());
      CHECK_FALSE(cubeSize.isLocked());
    });
}

TEST_CASE("Ordinary parent parameters dynamically control included parameters",
          "[customizer][parameter-attributes][dynamic][include]")
{
  TemporaryCustomizerDirectory files;
  files.writeFile("dynamic_attributes_include_normal.scad", R"(
@{
  hidden: selector != "show",
  locked: selector == "hide"
}
included_cube_size = 36;
)");

  const std::string source = selectorAttributes + R"(
selector = "hide";

include <dynamic_attributes_include_normal.scad>
)";

  withParsedParameters(
    source, files.path() / "dynamic_attributes_main_normal.scad",
    [](ParameterObjects& parameters, Context *context) {
      auto& selector = parameterNamed(parameters, "selector");
      auto& cubeSize = parameterNamed(parameters, "included_cube_size");

      updateParameterStates(parameters, context);
      CHECK(cubeSize.isHidden());
      CHECK(cubeSize.isLocked());

      setStringValue(selector, "show");
      updateParameterStates(parameters, context);
      CHECK_FALSE(cubeSize.isHidden());
      CHECK_FALSE(cubeSize.isLocked());
    });
}

// Keep the FileContext handle active, as CustomizerContext does, because
// special variables use EvaluationSession's dynamic context stack.
TEST_CASE("Special parent parameters dynamically control included parameters",
          "[customizer][parameter-attributes][dynamic][include][special-variable]")
{
  TemporaryCustomizerDirectory files;
  files.writeFile("dynamic_attributes_include_special.scad", R"(
@{
  hidden: $selector != "show"
}
special_included_cube_size = 36;
)");

  const std::string source = selectorAttributes + R"(
$selector = "show";

include <dynamic_attributes_include_special.scad>
)";

  withParsedParameters(
    source, files.path() / "dynamic_attributes_main_special.scad",
    [](ParameterObjects& parameters, Context *context) {
      auto& selector = parameterNamed(parameters, "$selector");
      auto& cubeSize = parameterNamed(parameters, "special_included_cube_size");

      CHECK(cubeSize.getDependencies() == std::set<std::string>{"$selector"});

      CHECK_FALSE(cubeSize.isHidden());

      setStringValue(selector, "hide");
      updateParameterStates(parameters, context);
      CHECK(cubeSize.isHidden());

      setStringValue(selector, "show");
      updateParameterStates(parameters, context);
      CHECK_FALSE(cubeSize.isHidden());
    });
}

TEST_CASE("SurfaceNode 8-bit and 16-bit PNG import", "[core][SurfaceNode]")
{
  SECTION("8-bit PNG import preserves standard height scaling")
  {
    auto path_8bit = get_test_image("surface_test_8bit.png");

    SurfaceNode node;
    img_data_t data = node.read_png_or_dat(path_8bit);

    REQUIRE(data.width == 2);
    REQUIRE(data.height == 2);

    // convert_image flips y: row 0 in data is bottom row of image (y=1)
    // image pixels: [0, 85, 170, 255]
    // y=1: x=0 -> 170, x=1 -> 255
    // y=0: x=0 -> 0,   x=1 -> 85
    CHECK(data[0] == Catch::Approx(100.0 / 65535.0 * 43690.0).margin(1e-4));
    CHECK(data[1] == Catch::Approx(100.0 / 65535.0 * 65535.0).margin(1e-4));
    CHECK(data[2] == Catch::Approx(100.0 / 65535.0 * 0.0).margin(1e-4));
    CHECK(data[3] == Catch::Approx(100.0 / 65535.0 * 21845.0).margin(1e-4));
  }

  SECTION("16-bit PNG import preserves 16-bit depth precision")
  {
    auto path_16bit = get_test_image("surface_test_16bit.png");

    SurfaceNode node;
    img_data_t data = node.read_png_or_dat(path_16bit);

    REQUIRE(data.width == 2);
    REQUIRE(data.height == 1);

    double expected_z0 = 100.0 / 65535.0 * 32768.0;
    double expected_z1 = 100.0 / 65535.0 * 32769.0;

    // Verify 16-bit precision: z0 and z1 must be distinct and match 16-bit scaling
    CHECK(data[0] == Catch::Approx(expected_z0).margin(1e-6));
    CHECK(data[1] == Catch::Approx(expected_z1).margin(1e-6));
    CHECK(data[1] > data[0]);
  }

  SECTION("invert scales identically for 8-bit and 16-bit inputs")
  {
    auto path_8bit = get_test_image("surface_test_invert_8bit.png");
    auto path_16bit = get_test_image("surface_test_invert_16bit.png");

    SurfaceNode node;
    node.invert = true;
    img_data_t data_8 = node.read_png_or_dat(path_8bit);
    img_data_t data_16 = node.read_png_or_dat(path_16bit);

    // invert must mirror about 0.0 to minimize breaking changes from the original 1.0 mirroring.
    // 8-bit image has pixel value 64. 64 -> 16448
    // 16-bit image has pixel value 16449.
    CHECK(data_8[0] == Catch::Approx(100.0 / 65535.0 * (0.0 - 16448.0)).margin(1e-4));
    CHECK(data_16[0] == Catch::Approx(100.0 / 65535.0 * (0.0 - 16449.0)).margin(1e-4));
  }
}
