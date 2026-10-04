// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/InsertionTestFixture.h"
#include "core/insertion/PlacementDraft.h"
#include "core/providers/CatalogLoader.h"
using namespace InsertionTest;
namespace {
Providers::GeneratedFragment peg(){
  auto json=nlohmann::json::parse(EvaluationInputs::diskBytes(std::filesystem::u8path(OPENSCAD_PROVIDER_TEST_DIR)/"builtin.json"));auto item=json["items"][2];item["id"]="peg";item["insertionRole"]="component";item["label"]="Test peg";json["items"]=nlohmann::json::array({item});json["providerId"]="fixture.component";
  auto loaded=Providers::loadCatalog(json.dump(),"fixture-component.json");REQUIRE(loaded);auto snapshot=std::make_shared<Providers::RegistrySnapshot>();snapshot->providers.push_back(*loaded.value);for(const auto& value:loaded.value->items)snapshot->items.emplace(value.qualifiedId,value);
  Providers::ProviderBinding binding{snapshot,"fixture.component:peg",loaded.value->digest,"","",0,{}};auto generated=Providers::ProviderRegistry::generate(binding,{{"diameter",4.0},{"height",12.0}},{});REQUIRE(generated);return std::move(*generated.value);
}
SourceTarget componentTarget(Fixture& f){auto options=f.inventory->targets(f.node("cube"),true,f.documents);REQUIRE(options.size()==1);if(options[0].diagnostic)INFO(options[0].diagnostic->message);REQUIRE(options[0].target);return *options[0].target;}
const std::string part="@{type: Part,id:\"plate\"}module plate(width=20){@{type: Anchor}mount={id:\"mount\",position:[width/2,10,10],normal:[0,0,1],up:[0,1,0]};cube([width,20,10]);}\n";
const std::string assembly=part+"@{type: Assembly,id:\"fixture\"}module fixture(width=20){@{type: Component,id:\"plate\"}plate(width=width);}\n@{type: Component,id:\"fixture-a\"}fixture(width=20);translate([50,0,0]){@{type: Component,id:\"fixture-b\"}fixture(width=30);}";
std::map<std::string,Transform3d> pieceFrames(const PreviewResponse& response){std::map<std::string,Transform3d> out;for(const auto& piece:response.meshes->pieces)if(auto path=response.piecePaths.find(piece.drawId);path!=response.piecePaths.end())out.emplace(path->second,piece.localToWorld);return out;}
}
TEST_CASE("placement_uses_provider_contact_mate_flip_and_unspun_offsets","[insertion-placement]"){
  Fixture f;f.evaluate("cube([20,20,10]);");auto target=f.target();for(const auto& id:{"scadmaxx.builtin:cube","scadmaxx.builtin:sphere","scadmaxx.builtin:plain-hole"}){
    auto fragment=f.fragment(id);auto draft=PlacementDraft::capture(target,fragment,{10,10,10},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic);REQUIRE(draft);draft.value->offset={2,3,4};draft.value->spinDegrees=37;auto matrix=draft.value->localPlacement(fragment);REQUIRE(matrix);
    const Vector3d contact(fragment.frame.origin[0],fragment.frame.origin[1],fragment.frame.origin[2]),normal(fragment.frame.normal[0],fragment.frame.normal[1],fragment.frame.normal[2]);CHECK((*matrix.value*contact).isApprox(Vector3d(12,13,14),1e-10));CHECK((matrix.value->linear()*normal).isApprox(-Vector3d::UnitZ(),1e-10));draft.value->flipped=true;auto flipped=draft.value->localPlacement(fragment);REQUIRE(flipped);CHECK((*flipped.value*contact).isApprox(Vector3d(12,13,14),1e-10));CHECK((flipped.value->linear()*normal).isApprox(Vector3d::UnitZ(),1e-10));
  }
}
TEST_CASE("fixed_local_capture_handles_mirrored_nonuniform_frames_and_rejects_singular","[insertion-placement]"){
  Fixture f;f.evaluate("scale([-2,3,4])cube(10);");auto target=f.target();auto fragment=f.fragment("scadmaxx.builtin:plain-hole");auto draft=PlacementDraft::capture(target,fragment,{-10,15,40},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic);REQUIRE(draft);CHECK(draft.value->fixedTarget.position.isApprox(Vector3d(5,5,10)));auto world=draft.value->worldPlacement(target,fragment,f.semantic);REQUIRE(world);CHECK(((*world.value)*Vector3d::Zero()).isApprox(Vector3d(-10,15,40)));
  target.localToWorld.linear().col(0).setZero();CHECK_FALSE(PlacementDraft::capture(target,fragment,{},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic));
}
TEST_CASE("plane_drag_reports_parallel_behind_and_near_zero_spin_without_mutation","[insertion-placement]"){
  Fixture f;f.evaluate("cube(10);");auto target=f.target();auto fragment=f.fragment("scadmaxx.builtin:cube");auto draft=PlacementDraft::capture(target,fragment,{5,5,10},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic);REQUIRE(draft);draft.value->offset={2,3,7};auto point=draft.value->planeCoordinates(target,f.semantic,{8,9,30},{0,0,-1});REQUIRE(point);CHECK(point.value->isApprox(Vector2d(3,4)));CHECK_FALSE(draft.value->planeCoordinates(target,f.semantic,{0,0,30},{1,0,0}));CHECK_FALSE(draft.value->planeCoordinates(target,f.semantic,{0,0,30},{0,0,1}));CHECK_FALSE(PlacementDraft::spinAngle({2,3},{2,3}));CHECK(*PlacementDraft::spinAngle({2,4},{2,3})==90);CHECK(draft.value->offset==Vector3d(2,3,7));
}
TEST_CASE("ordinary_component_stays_separate_and_recreates_fixed_document_placement","[insertion-placement]"){
  Fixture f;const std::string bytes="cube([20,20,10]);\n";f.evaluate(bytes);auto fragment=peg();auto target=componentTarget(f);CHECK(target.kind==SourceTarget::Kind::DocumentBody);auto draft=PlacementDraft::capture(target,fragment,{10,10,10},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic);REQUIRE(draft);auto plan=InsertionPlanner::component(*f.inventory,target,fragment,*draft.value,false);if(!plan)INFO(plan.diagnostic->message);REQUIRE(plan);CHECK(plan.value->candidate.find(bytes)!=std::string::npos);CHECK(plan.value->candidate.find("type: Assembly")==std::string::npos);auto response=f.preview(*plan.value);success(response);REQUIRE(response.meshes->pieces.size()==2);CHECK(bounds(response).max().isApprox(Vector3d(20,20,22)));CHECK(EvaluationInputs::diskBytes(f.directory/"root.scad")==bytes);
  Fixture independent;independent.evaluate(plan.value->candidate);REQUIRE(independent.semantic->instances.size()==1);auto instance=independent.semantic->instances.begin()->second;CHECK(instance.localToWorld.translation().isApprox(Vector3d(10,10,10)));
}
TEST_CASE("shared_named_component_uses_relative_endpoints_and_follows_only_changed_parameters","[insertion-placement]"){
  Fixture f;f.evaluate(assembly);auto target=componentTarget(f);REQUIRE(target.shared);auto fragment=peg();PlacementDraft draft;draft.mode=PlacementDraft::Mode::NamedAttachment;draft.namedTarget=Anchors::ReferenceKey{{Anchors::OwnerRole::Instance,"/fixture-a/plate"},"mount"};
  CHECK_FALSE(InsertionPlanner::component(*f.inventory,target,fragment,draft,false));auto plan=InsertionPlanner::component(*f.inventory,target,fragment,draft,true);if(!plan)INFO(plan.diagnostic->message);REQUIRE(plan);CHECK(plan.value->candidate.find("target: {component: \"plate\", anchor: \"mount\"}")!=std::string::npos);CHECK(plan.value->candidate.find("/fixture-a/plate")==std::string::npos);auto response=f.preview(*plan.value);success(response);auto frames=pieceFrames(response);REQUIRE(frames.count("/fixture-a/smx_insert_component_1"));REQUIRE(frames.count("/fixture-b/smx_insert_component_1"));CHECK(frames.at("/fixture-a/smx_insert_component_1").translation().isApprox(Vector3d(10,10,10)));CHECK(frames.at("/fixture-b/smx_insert_component_1").translation().isApprox(Vector3d(65,10,10)));
  Fixture changed;auto source=plan.value->candidate;const auto at=source.find("fixture(width=20);");REQUIRE(at!=std::string::npos);source.replace(at,std::string("fixture(width=20);").size(),"fixture(width=24);");changed.evaluate(source);CHECK(changed.semantic->instances.at("/fixture-a/smx_insert_component_1").localToWorld.translation().isApprox(Vector3d(12,10,10)));CHECK(changed.semantic->instances.at("/fixture-b/smx_insert_component_1").localToWorld.translation().isApprox(Vector3d(65,10,10)));
}
TEST_CASE("fixed_semantic_component_authors_one_owner_local_anchor_and_attachment","[insertion-placement]"){
  Fixture f;f.evaluate(assembly);auto target=componentTarget(f);auto fragment=peg();auto draft=PlacementDraft::capture(target,fragment,{10,10,10},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic,"/fixture-a/plate");REQUIRE(draft);auto plan=InsertionPlanner::component(*f.inventory,target,fragment,*draft.value,true);if(!plan)INFO(plan.diagnostic->message);REQUIRE(plan);CHECK(plan.value->candidate.find("owner: \"plate\"")!=std::string::npos);CHECK(plan.value->candidate.find("capture: \"fixed\"")!=std::string::npos);CHECK(plan.value->candidate.find("position:[width/2,10,10]")!=std::string::npos);auto response=f.preview(*plan.value);success(response);auto frames=pieceFrames(response);CHECK(frames.at("/fixture-a/smx_insert_component_1").translation().isApprox(Vector3d(10,10,10)));CHECK(frames.at("/fixture-b/smx_insert_component_1").translation().isApprox(Vector3d(60,10,10)));
  Fixture changed;auto source=plan.value->candidate;const auto at=source.find("fixture(width=20);");source.replace(at,std::string("fixture(width=20);").size(),"fixture(width=24);");changed.evaluate(source);CHECK(changed.semantic->instances.at("/fixture-a/smx_insert_component_1").localToWorld.translation().isApprox(Vector3d(10,10,10)));
}
TEST_CASE("component_destinations_never_enter_parts_and_named_shared_scope_is_checked","[insertion-placement]"){
  Fixture f;f.evaluate(assembly);auto target=f.target();auto fragment=peg();PlacementDraft draft;draft.mode=PlacementDraft::Mode::NamedAttachment;draft.namedTarget=Anchors::ReferenceKey{{Anchors::OwnerRole::Instance,"/fixture-a/plate"},"mount"};auto rejected=InsertionPlanner::component(*f.inventory,target,fragment,draft,true);REQUIRE_FALSE(rejected);CHECK(rejected.diagnostic->code=="WrongDestination");auto assemblyTarget=componentTarget(f);draft.namedTarget->owner.id="/fixture-b/plate";rejected=InsertionPlanner::component(*f.inventory,assemblyTarget,fragment,draft,true);REQUIRE_FALSE(rejected);CHECK(rejected.diagnostic->code=="SharedReferenceScope");
}
TEST_CASE("component_names_respect_imported_symbols_and_each_semantic_namespace","[insertion-placement]"){
  Fixture f;f.write("symbols.scad","module smx_insert_part_1(){}\n");
  f.evaluate("use <symbols.scad>\nsmx_insert_contact_1=0;smx_insert_anchor_1=0;smx_insert_attachment_1=0;\n"
    "@{type: Part,id:\"smx_insert_definition_1\"}module plate(){@{type: Anchor}mount={id:\"smx_insert_anchor_1\",position:[10,10,10],normal:[0,0,1],up:[0,1,0]};cube([20,20,10]);}\n"
    "@{type: Part,id:\"spare\"}module spare(){@{type: Anchor}contact={id:\"contact\",position:[0,0,0],normal:[0,0,1],up:[0,1,0]};sphere(1);}\n"
    "@{type: Assembly,id:\"fixture\"}module fixture(){@{type: Component,id:\"plate\"}plate();@{type: Component,id:\"smx_insert_component_1\"}spare();@{type: Attachment}old={id:\"smx_insert_attachment_1\",child:{component:\"smx_insert_component_1\",anchor:\"contact\"},target:{component:\"plate\",anchor:\"smx_insert_anchor_1\"},mode:\"align\"};}\n"
    "@{type: Component,id:\"fixture\"}fixture();");
  auto fragment=peg();auto target=componentTarget(f);auto draft=PlacementDraft::capture(target,fragment,{10,10,10},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic,"/fixture/plate");REQUIRE(draft);
  auto authored=InsertionPlanner::component(*f.inventory,target,fragment,*draft.value,false);INFO((authored.diagnostic?authored.diagnostic->message:""));REQUIRE(authored);
  for(const auto& text:{"module smx_insert_part_2","smx_insert_contact_2 =","id: \"smx_insert_definition_2\"","id: \"smx_insert_component_2\"","smx_insert_anchor_2 = {id: \"smx_insert_anchor_2\"","smx_insert_attachment_2 = {id: \"smx_insert_attachment_2\""})CHECK(authored.value->candidate.find(text)!=std::string::npos);
  auto response=f.preview(*authored.value);success(response);CHECK(pieceFrames(response).count("/fixture/smx_insert_component_2"));
}
TEST_CASE("shared_component_source_preserves_reflected_nonuniform_owner_dimensions","[insertion-placement]"){
  Fixture f;const auto source=part+"@{type: Assembly,id:\"fixture\"}module fixture(width=20){@{type: Component,id:\"plate\"}plate(width=width);}\nscale([-2,3,4]){@{type: Component,id:\"fixture-a\"}fixture(width=20);}translate([100,0,0])scale([2,3,4]){@{type: Component,id:\"fixture-b\"}fixture(width=30);}";
  f.evaluate(source);auto target=componentTarget(f);auto fragment=peg();auto draft=PlacementDraft::capture(target,fragment,{-20,30,40},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic,"/fixture-a/plate");REQUIRE(draft);
  auto authored=InsertionPlanner::component(*f.inventory,target,fragment,*draft.value,true);REQUIRE(authored);auto response=f.preview(*authored.value);success(response);auto frames=pieceFrames(response);
  CHECK(frames.at("/fixture-a/smx_insert_component_1").translation().isApprox(Vector3d(-20,30,40)));
  CHECK(frames.at("/fixture-b/smx_insert_component_1").translation().isApprox(Vector3d(120,30,40)));
  for(const auto& path:{"/fixture-a/smx_insert_component_1","/fixture-b/smx_insert_component_1"})for(int axis=0;axis<3;++axis)CHECK(std::abs(frames.at(path).linear().col(axis).norm()-(axis==0?2:axis==1?3:4))<1e-10);
}
