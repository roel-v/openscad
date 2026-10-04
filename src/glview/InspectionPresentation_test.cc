#include "glview/InspectionPresentation.h"
#include <catch2/catch_test_macros.hpp>

namespace {
std::shared_ptr<InspectionMeshSet> fixture()
{
  auto result=std::make_shared<InspectionMeshSet>(); result->resultId="result"; result->geometryGeneration=3;
  auto mesh=std::make_shared<PolySet>(3);
  result->pieces={{uint64_t(1)<<40,{},mesh,Transform3d::Identity(),Color4f(1,0,0,1)},
    {7,{},mesh,Transform3d::Identity(),Color4f(0,1,0,1)}};
  return result;
}
}
TEST_CASE("Inspection provider priority and request lifetime retain explicit ownership", "[inspection-presentation]")
{
  InspectionPresentation presentation;
  const auto root=presentation.registerProvider("root",0);
  REQUIRE(root); REQUIRE_FALSE(presentation.requested());
  REQUIRE_FALSE(presentation.registerProvider("root",100));
  REQUIRE_FALSE(presentation.registerProvider("another",0));
  const auto request=presentation.request("clip");
  const auto first=presentation.beginJob(); REQUIRE(presentation.accepts(first,root));
  const auto part=presentation.registerProvider("parts",100);
  REQUIRE(presentation.selectedProvider()==part); REQUIRE_FALSE(presentation.accepts(first,root));
  const auto second=presentation.beginJob();
  REQUIRE(presentation.accepts(second,part)); REQUIRE_FALSE(presentation.accepts(second,root));
  const auto lower=presentation.registerProvider("lower",-1);
  REQUIRE(presentation.accepts(second,part)); // A lower provider cannot cancel or steal this job.
  presentation.removeProvider(lower); REQUIRE(presentation.accepts(second,part));
  const auto extra=presentation.request("visibility"); presentation.release(request);
  REQUIRE(presentation.accepts(second,part));
  presentation.release(extra); REQUIRE_FALSE(presentation.accepts(second,part));
  REQUIRE_FALSE(presentation.requested());
  presentation.request("new request"); REQUIRE(presentation.enableProvider(part,false));
  REQUIRE(presentation.selectedProvider()==root);
}
TEST_CASE("Inspection masks intersect exact IDs and reject stale sets", "[inspection-presentation]")
{
  InspectionPresentation presentation;
  const auto a=presentation.request("a"),b=presentation.request("b");
  auto set=fixture(); REQUIRE(presentation.install(set,"result"));
  const uint64_t wide=uint64_t(1)<<40;
  REQUIRE(presentation.visible()==std::set<uint64_t>{7,wide});
  REQUIRE_FALSE(presentation.setMask(a,{1,{7}},"stale",3));
  REQUIRE_FALSE(presentation.setMask(a,{1,{7}},"result",2));
  REQUIRE_FALSE(presentation.setMask(a,{1,{8}},"result",3));
  REQUIRE(presentation.setMask(a,{1,{wide}},"result",3));
  REQUIRE(presentation.visible()==std::set<uint64_t>{wide});
  REQUIRE_FALSE(presentation.setMask(a,{1,{7}},"result",3));
  REQUIRE(presentation.setMask(b,{1,{7}},"result",3)); REQUIRE(presentation.visible().empty());
  presentation.clearMask(b); REQUIRE(presentation.visible()==std::set<uint64_t>{wide});
  REQUIRE(presentation.setMask(a,{2,{}},"result",3)); REQUIRE(presentation.visible().empty());
  presentation.release(a); REQUIRE(presentation.visible()==std::set<uint64_t>{7,wide});
  presentation.invalidate(); REQUIRE_FALSE(presentation.meshes());
}
TEST_CASE("Inspection validates pieces and orders effects deterministically", "[inspection-presentation]")
{
  InspectionPresentation presentation;
  auto set=fixture(); set->pieces[1].drawId=set->pieces[0].drawId;
  REQUIRE_FALSE(presentation.install(set,"result"));
  set=fixture(); set->pieces[1].drawId=0; REQUIRE_FALSE(presentation.install(set,"result"));
  set=fixture(); set->pieces[0].localToWorld.matrix()(3,0)=1; REQUIRE_FALSE(presentation.install(set,"result"));
  set=fixture(); REQUIRE_FALSE(presentation.install(set,"different"));
  REQUIRE(presentation.install(set,"result"));
  const auto c=presentation.registerEffect("c",2),b=presentation.registerEffect("b",1),a=presentation.registerEffect("a",1);
  REQUIRE_FALSE(presentation.registerEffect("a",4));
  REQUIRE(presentation.orderedEffects()==std::vector<uint64_t>{a,b,c});
  presentation.removeEffect(b); REQUIRE(presentation.orderedEffects()==std::vector<uint64_t>{a,c});
}
