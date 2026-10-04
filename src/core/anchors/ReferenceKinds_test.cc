#include "core/anchors/ReferenceKinds.h"
#include "core/design/DesignKinds.h"
#include "core/semantic/SemanticTestFixture.h"
#include <catch2/catch_approx.hpp>
#include <limits>
using namespace Anchors;
namespace {
Value vector(std::initializer_list<double> numbers) { VectorType out(nullptr); for(double n:numbers) out.emplace_back(Value(n)); return Value(std::move(out)); }
Value anchorValue() {
  ObjectType object(nullptr); object.set("id",Value("a")); object.set("position",vector({1,2,3}));
  object.set("normal",vector({0,0,4})); object.set("up",vector({0,3,2})); return Value(std::move(object));
}
bool valid(const std::string& kind,const Value& value) { return bool(decodeReference(kind,value,{}).payload); }
}
TEST_CASE("Anchor schemas normalize orientation and decode every supported adjustment", "[anchor-schema]") {
  auto value = anchorValue(); auto object = value.toObject();
  object.set("owner",Value("/plate")); object.set("label",Value("Mount")); object.set("spin",Value(450.0));
  object.set("capture",Value("fixed")); object.set("purpose",Value("drawing"));
  ObjectType adjust(nullptr); adjust.set("translation",vector({4,5,6})); adjust.set("rotation",vector({0,0,2,2})); object.set("adjust",Value(std::move(adjust)));
  auto decoded = decodeReference("Anchor",Value(std::move(object)),{}); REQUIRE(decoded.diagnostics.empty());
  auto result = std::dynamic_pointer_cast<const ReferenceDeclaration>(decoded.payload); REQUIRE(result);
  const auto& anchor = std::get<Anchor>(result->value);
  REQUIRE(anchor.normal.isApprox(Vector3d::UnitZ())); REQUIRE(anchor.up.isApprox(Vector3d::UnitY()));
  REQUIRE(anchor.translation.isApprox(Vector3d(4,5,6))); REQUIRE(anchor.rotation.norm() == Catch::Approx(1));
  REQUIRE(anchor.spin == 450); REQUIRE(anchor.fixedCapture); REQUIRE(anchor.drawingFrame); REQUIRE(result->owner == "/plate");
}
TEST_CASE("Reference schemas reject unknown duplicate nonfinite and unavailable orientation fields", "[anchor-schema]") {
  auto base = anchorValue();
  SECTION("unknown") { auto object=base.toObject();object.set("nearest",Value(true));REQUIRE_FALSE(valid("Anchor",Value(std::move(object)))); }
  SECTION("duplicate") { auto object=base.toObject();object.recordLiteralDuplicateKey("normal");REQUIRE_FALSE(valid("Anchor",Value(std::move(object)))); }
  SECTION("nonfinite") { auto object=base.toObject();object.set("position",vector({0,0,std::numeric_limits<double>::infinity()}));REQUIRE_FALSE(valid("Anchor",Value(std::move(object)))); }
  SECTION("zero") { auto object=base.toObject();object.set("normal",vector({0,0,0}));REQUIRE_FALSE(valid("Anchor",Value(std::move(object)))); }
  SECTION("parallel") { auto object=base.toObject();object.set("up",vector({0,0,1}));REQUIRE_FALSE(valid("Anchor",Value(std::move(object)))); }
  SECTION("quaternion") { auto object=base.toObject();ObjectType adjust(nullptr);adjust.set("rotation",vector({0,0,0,0}));object.set("adjust",Value(std::move(adjust)));REQUIRE_FALSE(valid("Anchor",Value(std::move(object)))); }
  SECTION("adjust duplicate") { auto object=base.toObject();ObjectType adjust(nullptr);adjust.set("translation",vector({0,0,0}));adjust.recordLiteralDuplicateKey("translation");object.set("adjust",Value(std::move(adjust)));REQUIRE_FALSE(valid("Anchor",Value(std::move(object)))); }
}
TEST_CASE("Point Axis and Plane retain their distinct mathematical types", "[anchor-schema]") {
  for (const std::string kind : {"Point","Axis","Plane"}) {
    ObjectType object(nullptr);object.set("id",Value("a"));object.set("position",vector({1,2,3}));
    if(kind=="Axis")object.set("direction",vector({2,0,0}));if(kind=="Plane")object.set("normal",vector({0,0,-3}));
    auto decoded=decodeReference(kind,Value(std::move(object)),{});REQUIRE(decoded.payload);
    auto result=std::dynamic_pointer_cast<const ReferenceDeclaration>(decoded.payload);
    REQUIRE_FALSE(std::holds_alternative<Anchor>(result->value));
    REQUIRE((kind=="Point" ? std::holds_alternative<Point>(result->value) : kind=="Axis" ? std::holds_alternative<Axis>(result->value) : std::holds_alternative<Plane>(result->value)));
  }
}
TEST_CASE("Attachment schemas validate endpoint objects and literal adjustments", "[anchor-schema]") {
  SemanticTest::Fixture fixture;
  fixture.evaluate("@{type:Attachment} a={id:\"a\",child:{component:\"c\",anchor:\"base\",anchor:\"other\"},target:{component:\"p\",anchor:\"top\"}};cube(1);");
  REQUIRE_FALSE(fixture.node); REQUIRE(fixture.diagnostic("attachment-invalid"));
  auto kind=Design::kind("Attachment");REQUIRE(kind);
  Design::DeclarationContext context;context.source.host=Design::DeclarationHost::ModuleCall;
  std::vector<Design::Diagnostic> diagnostics; REQUIRE_FALSE(Design::makeRecord(*kind,anchorValue(),context,diagnostics));REQUIRE(diagnostics.front().code=="unsupported-host-scope");
}
