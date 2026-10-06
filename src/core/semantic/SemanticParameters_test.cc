#include "core/semantic/SemanticTestFixture.h"
#include "utils/printutils.h"
using SemanticTest::Fixture;
TEST_CASE("Semantic parameter observation preserves native winning arguments defaults and ambiguity", "[semantic-evaluation]") {
  Fixture fixture; fixture.evaluate("base=7; @{type:Part,id:\"p\"} module p(a=base,b=8,c=9){cube([a,b,c]);} @{type:Component,id:\"positional\"} p(1,2); @{type:Component,id:\"named\"} p(b=3,4); @{type:Component,id:\"duplicate\"} p(1,a=5,a=6); @{type:Component,id:\"defaults\"} p();");
  REQUIRE(fixture.scene->valid());
  const auto& positional = fixture.scene->instances.at("/positional").parameters;
  REQUIRE(positional.at("a").value->toDouble() == 1); REQUIRE(positional.at("b").value->toDouble() == 2);
  REQUIRE(positional.at("c").fromDefault); REQUIRE(positional.at("c").source->originalExpression == "9");
  const auto& named = fixture.scene->instances.at("/named").parameters;
  REQUIRE(named.at("a").value->toDouble() == 4); REQUIRE(named.at("a").winningArgument == 1); REQUIRE(named.at("b").winningArgument == 0);
  const auto& duplicate = fixture.scene->instances.at("/duplicate").parameters.at("a");
  REQUIRE(duplicate.value->toDouble() == 6); REQUIRE(duplicate.winningArgument == 2); REQUIRE(duplicate.ambiguous); REQUIRE(duplicate.source->originalExpression == "6");
  const auto& defaults = fixture.scene->instances.at("/defaults").parameters.at("a");
  REQUIRE(defaults.value->toDouble() == 7); REQUIRE(defaults.fromDefault); REQUIRE(defaults.source->originalExpression == "base");
}
TEST_CASE("Semantic argument metadata and default expressions execute once during native binding", "[semantic-evaluation]") {
  int observations=0; auto oldHandler=outputhandler; auto oldData=outputhandler_data;
  struct Restore {OutputHandlerFunc *handler;void *data;~Restore(){set_output_handler(handler,nullptr,data);}} restore{oldHandler,oldData};
  set_output_handler([](const Message& value,void *data){if(value.msg.find("ONCE_")!=std::string::npos)++*static_cast<int *>(data);},nullptr,&observations);
  Fixture fixture; fixture.evaluate("@{type:Part,id:\"p\"} module p(a=echo(\"ONCE_DEFAULT\") 8,b=9){cube([a,b,1]);} @{type:Component,id:echo(\"ONCE_ID\") \"a\"} p(b=echo(\"ONCE_ARG\") 12);");
  REQUIRE(fixture.scene->valid()); REQUIRE(observations == 3);
  REQUIRE(fixture.scene->instances.at("/a").parameters.at("a").value->toDouble() == 8);
  REQUIRE(fixture.bounds(fixture.node).max().isApprox(Vector3d(8,12,1)));
}
TEST_CASE("Semantic nonliteral parameters retain immutable data or an explicit readonly value", "[semantic-evaluation]") {
  Fixture fixture; fixture.evaluate("@{type:Part,id:\"p\"} module p(data={a:[1,2]},fn=function(x)x,range=[1:3]){cube(1);} @{type:Component,id:\"a\"} p();");
  REQUIRE(fixture.scene->valid()); const auto& parameters=fixture.scene->instances.at("/a").parameters;
  REQUIRE(parameters.at("data").value); REQUIRE((parameters.at("data").type == Value::Type::OBJECT));
  REQUIRE_FALSE(parameters.at("fn").value); REQUIRE((parameters.at("fn").type == Value::Type::FUNCTION)); REQUIRE_FALSE(parameters.at("fn").displayedValue.empty());
  REQUIRE_FALSE(parameters.at("range").value); REQUIRE((parameters.at("range").type == Value::Type::RANGE));
}
