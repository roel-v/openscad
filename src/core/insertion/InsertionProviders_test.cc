// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/InsertionTestFixture.h"
#include "core/insertion/PlacementDraft.h"
#include "core/parsersettings.h"
#include "core/providers/ContentDigest.h"
using namespace InsertionTest;
namespace {
struct Libraries {
  NativeExecutionGuard gate;
  std::vector<std::string> old=get_library_path();
  Libraries(){auto bosl=qEnvironmentVariable("SCADMAXX_PROVIDER_BOSL2_ROOT"),bolts=qEnvironmentVariable("SCADMAXX_PROVIDER_BOLTS_ROOT");if(bosl.isEmpty() || bolts.isEmpty())SKIP("Pinned external libraries must be supplied explicitly");set_library_path({EvaluationInputs::fileKey(std::filesystem::u8path(bosl.toStdString()).parent_path()),EvaluationInputs::fileKey(std::filesystem::u8path(bolts.toStdString()).parent_path())});}
  ~Libraries(){set_library_path(std::move(old));}
};
Providers::GeneratedFragment generate(Fixture& f,const std::string& id){return f.fragment(id,id.find("builtin")==std::string::npos?Providers::ParameterValues{{"size",std::string("M4")}}:Providers::ParameterValues{});}
EditPlan plan(Fixture& f,const Providers::GeneratedFragment& fragment){
  const bool component=fragment.insertionRole==Providers::InsertionRole::Component;
  auto choices=f.inventory->targets(f.node("cube"),component,f.documents);REQUIRE(!choices.empty());REQUIRE(choices[0].target);auto target=*choices[0].target;
  auto placement=PlacementDraft::capture(target,fragment,{10,10,10},Vector3d::UnitZ(),Vector3d::UnitY(),f.semantic);REQUIRE(placement);
  auto result=component?InsertionPlanner::component(*f.inventory,target,fragment,*placement.value,false):InsertionPlanner::geometry(*f.inventory,target,fragment,*placement.value->localPlacement(fragment).value,false);if(!result)INFO(result.diagnostic->message);REQUIRE(result);return std::move(*result.value);
}
BoundingBox itemBounds(const PreviewResponse& response){REQUIRE(response.itemMeshes);BoundingBox out;for(const auto& piece:response.itemMeshes->pieces)for(const auto& vertex:piece.mesh->vertices)out.extend(piece.localToWorld*vertex);return out;}
}
TEST_CASE("all_eight_authored_candidates_preserve_contacts_and_real_library_closure","[insertion-providers]"){
  Libraries libraries;auto registry=Providers::ProviderRegistry::load({{OPENSCAD_PROVIDER_TEST_DIR}});REQUIRE(registry->items.size()==8);
  for(const auto& [id,item]:registry->items){INFO(id);Fixture f;f.configuration.libraryPaths=get_library_path();f.evaluate("$fn=48;\ncube([20,20,10]);\n");auto fragment=generate(f,id);auto authored=plan(f,fragment);auto response=f.preview(authored);success(response);
    for(const auto& library:fragment.binding.libraries)for(const auto& file:library.files)CHECK(response.consumed.at(file.canonicalPath)==EvaluationInputs::fingerprint(EvaluationInputs::diskBytes(std::filesystem::u8path(file.canonicalPath))));
    if(item.operation==Providers::Operation::Subtractive){REQUIRE(response.volumes.size()==1);CHECK(response.volumes[0].after<response.volumes[0].before-10);CHECK(bounds(response).max().isApprox(Vector3d(20,20,10),1e-8));}
    if(id=="scadmaxx.bolts:iso4017"){CHECK(std::abs(itemBounds(response).max().z()-20)<1e-6);CHECK(std::abs(itemBounds(response).min().z()+2.8)<1e-6);CHECK(std::abs(bounds(response).min().z()+10)<1e-6);CHECK(std::abs(bounds(response).max().z()-12.8)<1e-6);}
    if(id=="scadmaxx.bolts:iso4032")CHECK(std::abs(bounds(response).max().z()-13.2)<1e-6);
    if(id=="scadmaxx.bolts:iso7089")CHECK(std::abs(bounds(response).max().z()-10.8)<1e-6);
  }
}
TEST_CASE("authored_external_insertions_isolate_hostile_ambient_controls","[insertion-providers]"){
  Libraries libraries;
  for(const auto& id:{"scadmaxx.bosl2:threaded-hole","scadmaxx.bolts:iso4017","scadmaxx.bolts:iso4032","scadmaxx.bolts:iso7089"}){INFO(id);std::vector<PreviewResponse> results;
    for(bool hostile:{false,true}){Fixture f;f.configuration.libraryPaths=get_library_path();f.evaluate(std::string("$fn=48;\n")+(hostile?"BOLTS_MODE=\"bom\";BOLTS_DEFAULT_UNIT=\"in\";$slop=2;\n":"")+"cube([20,20,10]);\n");auto fragment=generate(f,id);results.push_back(f.preview(plan(f,fragment)));success(results.back());}
    CHECK(bounds(results[0]).min().isApprox(bounds(results[1]).min(),1e-8));CHECK(bounds(results[0]).max().isApprox(bounds(results[1]).max(),1e-8));auto a=itemBounds(results[0]),b=itemBounds(results[1]);CHECK(a.min().isApprox(b.min(),1e-8));CHECK(a.max().isApprox(b.max(),1e-8));
    if(!results[0].volumes.empty())CHECK(std::abs(results[0].volumes[0].after-results[1].volumes[0].after)<1e-6);
  }
}
TEST_CASE("authored_provider_rejects_dirty_profile_and_native_local_shadow","[insertion-providers]"){
  Libraries libraries;Fixture f;f.configuration.libraryPaths=get_library_path();f.evaluate("module threaded_rod(){cube(1);}\ncube([20,20,10]);");auto registry=Providers::ProviderRegistry::load({{OPENSCAD_PROVIDER_TEST_DIR}});auto context=Providers::nativeResolutionContext(EvaluationInputs::fileKey(f.directory));REQUIRE(context);auto binding=Providers::ProviderRegistry::bind(registry,"scadmaxx.bosl2:threaded-hole",*context.value);REQUIRE(binding);
  auto reader=context.value->readBytes;context.value->readBytes=[reader](const std::string& path){auto bytes=reader(path);if(bytes && path.find("screws.scad")!=std::string::npos){bytes.value->bytes+="\nmodule modified_profile_marker(){}\n";bytes.value->revision=Providers::sha256(bytes.value->bytes);}return bytes;};auto dirty=Providers::ProviderRegistry::bind(registry,"scadmaxx.bosl2:threaded-hole",*context.value);REQUIRE_FALSE(dirty);CHECK(dirty.diagnostic->code=="ModifiedLibrary");
  auto fragment=generate(f,"scadmaxx.bosl2:threaded-hole");const auto module=fragment.moduleBindings.front().module; // shadow the actual native provider binding after its include
  f.evaluate("module "+module+"(){cube(1);}\ncube([20,20,10]);");fragment=generate(f,"scadmaxx.bosl2:threaded-hole");auto response=f.preview(plan(f,fragment));CHECK(response.status==PreviewStatus::Invalid);CHECK_FALSE(response.meshes);
}
