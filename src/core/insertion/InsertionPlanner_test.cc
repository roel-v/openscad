// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/InsertionTestFixture.h"
using namespace InsertionTest;

TEST_CASE("generated_helpers_preserve_native_customizer_fields_and_override_values","[insertion-source]"){
  Fixture f;f.evaluate("$fn=48;\nwidth=20; // [10:40]\n// Preserve description.\ncube([width,20,10]);\n");
  boost::property_tree::ptree value;value.put_value("24");f.configuration.parameters.emplace("width",value);value.put_value("48");f.configuration.parameters.emplace("$fn",value);
  auto target=f.target();auto fragment=f.fragment("scadmaxx.builtin:plain-hole",{{"diameter",4.0},{"depth",12.0}});
  auto authored=InsertionPlanner::geometry(*f.inventory,target,fragment,placed({10,10,10},true),false);REQUIRE(authored);
  CHECK(authored.value->candidate.find("width=20; // [10:40]")<authored.value->candidate.find("module smx_insert_item_1"));
  auto response=f.preview(*authored.value);success(response);CHECK(bounds(response).max().isApprox(Vector3d(24,20,10)));
  f.evaluate("cube(10);\nwidth=20;\n");auto rejected=InsertionPlanner::geometry(*f.inventory,f.target(),f.fragment("scadmaxx.builtin:cube"),placed({0,0,10}),false);
  REQUIRE_FALSE(rejected);CHECK(rejected.diagnostic->code=="CustomizerScope");
}

TEST_CASE("source_golden_files_match_the_complete_candidate_bytes","[insertion-source]"){
  const auto fixtures=std::filesystem::u8path(OPENSCAD_PROVIDER_TEST_DIR).parent_path()/"tests/data/insertion";
  for(bool subtract:{false,true}){Fixture f;auto before=EvaluationInputs::diskBytes(fixtures/"ordinary-before.scad");f.evaluate(before);auto fragment=f.fragment(subtract?"scadmaxx.builtin:plain-hole":"scadmaxx.builtin:cube",subtract?Providers::ParameterValues{{"diameter",4.0},{"depth",12.0}}:Providers::ParameterValues{{"width",10.0},{"depth",10.0},{"height",30.0}});auto placement=placed(subtract?Vector3d(10,10,10):Vector3d(0,0,10));if(subtract)placement.linear().diagonal()<<-1,1,-1;
    auto planned=InsertionPlanner::geometry(*f.inventory,f.target(),fragment,placement,false);REQUIRE(planned);auto expected=EvaluationInputs::diskBytes(fixtures/(subtract?"ordinary-cut.expected.scad":"ordinary-add.expected.scad"));const auto marker=expected.find("// @PROVENANCE@\n");REQUIRE(marker!=std::string::npos);expected.replace(marker,std::string("// @PROVENANCE@\n").size(),fragment.provenanceComment);CHECK(planned.value->candidate==expected);success(f.preview(*planned.value));}
}

TEST_CASE("ordinary_cut_and_add_preserve_expressions_and_native_bounds","[insertion-source]"){
  Fixture f;const std::string source="// License and untouched expression.\n$fn=32;\nwidth=20;\ncube([width,20,10]); // final comment\n";f.evaluate(source);auto target=f.target();
  auto hole=f.fragment("scadmaxx.builtin:plain-hole",{{"diameter",4.0},{"depth",12.0}});auto cut=InsertionPlanner::geometry(*f.inventory,target,hole,placed({10,10,10},true),false);if(!cut)INFO(cut.diagnostic->message);REQUIRE(cut);CHECK(cut.value->candidate.find("width=20;\n")!=std::string::npos);CHECK(cut.value->candidate.find("cube([width,20,10]); // final comment\n")!=std::string::npos);auto response=f.preview(*cut.value);success(response);CHECK(bounds(response).max().isApprox(Vector3d(20,20,10)));REQUIRE(response.volumes.size()==1);CHECK(std::abs(response.volumes[0].before-4000)<1e-9);CHECK(response.volumes[0].after<3900);CHECK(response.volumes[0].after>3800);CHECK(EvaluationInputs::diskBytes(f.directory/"root.scad")==source);
  auto cube=f.fragment("scadmaxx.builtin:cube",{{"width",10.0},{"depth",10.0},{"height",30.0}});auto added=InsertionPlanner::geometry(*f.inventory,target,cube,placed({0,0,10}),false);REQUIRE(added);response=f.preview(*added.value);success(response);CHECK(bounds(response).max().isApprox(Vector3d(20,20,40)));
}
TEST_CASE("part_geometry_run_cuts_two_direct_solids_and_preserves_nested_boolean","[insertion-source]"){
  Fixture f;f.evaluate("@{type: Part,id:\"plate\"} module plate(width=20){\nheight=10;\ndifference(){cube([width,20,height]);translate([0,0,0])cube(1);}\ntranslate([20,0,0])cube([10,20,height]);\n}\n@{type: Component,id:\"plate\"} plate();");auto target=f.target();CHECK(target.kind==SourceTarget::Kind::PartBody);auto fragment=f.fragment("scadmaxx.builtin:plain-hole",{{"diameter",6.0},{"depth",12.0}});auto plan=InsertionPlanner::geometry(*f.inventory,target,fragment,placed({20,10,10},true),false);REQUIRE(plan);CHECK(plan.value->candidate.find("height=10;\n")!=std::string::npos);CHECK(plan.value->candidate.find("difference(){cube([width,20,height]);translate([0,0,0])cube(1);}")!=std::string::npos);auto response=f.preview(*plan.value);success(response);CHECK(bounds(response).max().isApprox(Vector3d(30,20,10)));CHECK(response.volumes.front().after<response.volumes.front().before-200);
}
TEST_CASE("source_splices_preserve_unicode_bom_crlf_and_unterminated_line_comment","[insertion-source]"){
  Fixture f;const std::string source="\xef\xbb\xbf// License café\r\nwidth=20;\r\ncube([width,20,10]); // final 測試";f.evaluate(source);auto target=f.target();auto plan=InsertionPlanner::geometry(*f.inventory,target,f.fragment("scadmaxx.builtin:plain-hole",{{"diameter",4.0},{"depth",12.0}}),placed({10,10,10},true),false);REQUIRE(plan);CHECK(plan.value->candidate.rfind("\xef\xbb\xbf// License café\r\n",0)==0);CHECK(plan.value->candidate.find("cube([width,20,10]); // final 測試\r\n")!=std::string::npos);for(size_t i=0;i<plan.value->candidate.size();++i)if(plan.value->candidate[i]=='\n')CHECK((i>0 && plan.value->candidate[i-1]=='\r'));success(f.preview(*plan.value));
}
TEST_CASE("interleaved_part_declarations_are_rejected_without_moving_source","[insertion-source]"){
  for(const auto& declaration:{"size=3;","module helper(){cube(2);}","function helper()=3;"}){
    Fixture f;const std::string bytes="@{type: Part,id:\"p\"} module p(){cube(10);"+std::string(declaration)+"translate([20,0,0])cube(10);}@{type: Component,id:\"p\"}p();";f.evaluate(bytes);auto choices=f.inventory->targets(f.node("cube"),false,f.documents);REQUIRE(choices.size()==1);REQUIRE(choices[0].diagnostic);CHECK(choices[0].diagnostic->code=="InterleavedDeclaration");CHECK(f.inputs->root().utf8==bytes);
  }
}
TEST_CASE("shared_part_scope_requires_approval_and_checks_every_instance","[insertion-source]"){
  Fixture f;f.evaluate("@{type: Part,id:\"plate\"} module plate(width=20){cube([width,20,10]);}\n@{type: Component,id:\"left\"}plate(width=20);translate([40,0,0]){@{type: Component,id:\"right\"}plate(width=30);}");auto target=f.target();REQUIRE(target.shared);REQUIRE(target.affectedPaths.size()==2);auto fragment=f.fragment("scadmaxx.builtin:plain-hole",{{"diameter",4.0},{"depth",12.0}});auto rejected=InsertionPlanner::geometry(*f.inventory,target,fragment,placed({10,10,10},true),false);REQUIRE_FALSE(rejected);CHECK(rejected.diagnostic->code=="SharedScopeRequired");auto accepted=InsertionPlanner::geometry(*f.inventory,target,fragment,placed({10,10,10},true),true);REQUIRE(accepted);auto response=f.preview(*accepted.value);success(response);CHECK(response.volumes.size()==2);CHECK(response.affectedPaths==target.affectedPaths);CHECK(response.meshes->pieces.size()==2);CHECK(bounds(response).max().isApprox(Vector3d(70,20,10)));
}
TEST_CASE("ordinary_repeated_calls_offer_only_proven_unique_enclosing_statement","[insertion-source]"){
  Fixture f;f.evaluate("for(x=[0:2])translate([x*20,0,0])cube(10);");auto options=f.inventory->targets(f.node("cube"),false,f.documents);bool repeated=false,enclosing=false;for(const auto& option:options){if(option.diagnostic && option.diagnostic->code=="RepeatedStatement")repeated=true;if(option.target && option.target->label=="for")enclosing=true;if(option.target)CHECK(option.target->label!="cube");}CHECK(repeated);CHECK(enclosing);
}
TEST_CASE("negative_boundaries_offer_positive_boolean_ancestors_and_forbid_modifiers","[insertion-source]"){
  Fixture difference;difference.evaluate("difference(){cube(10);translate([2,2,-1])cube([6,6,12]);}");auto options=difference.inventory->targets(difference.node("cube",1),false,difference.documents);bool positive=false,rejected=false;for(const auto& option:options){if(option.target && option.target->label=="difference")positive=true;if(option.diagnostic && option.diagnostic->code=="NonPositiveTarget")rejected=true;}CHECK(positive);CHECK(rejected);
  for(const auto& modifier:{"!","#","%"}){Fixture f;f.evaluate(std::string(modifier)+"cube(10);");auto choices=f.inventory->targets(f.node("cube"),false,f.documents);if(std::string(modifier)=="#")CHECK(choices[0].target.has_value());else {CHECK_FALSE(choices[0].target);CHECK(choices[0].diagnostic.has_value());}}
}
TEST_CASE("included_target_is_explicit_open_user_source_and_preview_uses_original_root","[insertion-source]"){
  Fixture f;const std::string included="@{type: Part,id:\"p\"}module p(width=20){cube([width,20,10]);}";f.write("part.scad",included);const std::string root="include <part.scad>\ntranslate([50,0,0]){@{type: Component,id:\"p\"}p(width=30);}";f.evaluate(root,{{f.key("part.scad"),included,{},"included-editor",7}});
  auto choices=f.inventory->targets(f.node("cube"),false,f.documents);REQUIRE(choices[0].diagnostic);CHECK(choices[0].diagnostic->code=="OpenSource");
  f.documents[f.key("part.scad")]={true,false,true};choices=f.inventory->targets(f.node("cube"),false,f.documents);CHECK(choices[0].diagnostic->code=="ReadOnlySource");f.documents[f.key("part.scad")]={true,true,false};choices=f.inventory->targets(f.node("cube"),false,f.documents);CHECK(choices[0].diagnostic->code=="VendorSource");f.documents[f.key("part.scad")]={true,true,true};auto target=f.target();REQUIRE(target.region.fileKey==f.key("part.scad"));auto plan=InsertionPlanner::geometry(*f.inventory,target,f.fragment("scadmaxx.builtin:plain-hole",{{"diameter",4.0},{"depth",12.0}}),placed({10,10,10},true),false);REQUIRE(plan);auto response=f.preview(*plan.value);success(response);CHECK(bounds(response).min().x()==50);CHECK(bounds(response).max().x()==80);CHECK(EvaluationInputs::diskBytes(f.directory/"part.scad")==included);CHECK(f.inputs->root().utf8==root);
}
TEST_CASE("no_intersection_is_invalid_and_stale_original_bytes_never_apply","[insertion-source]"){
  Fixture f;f.evaluate("width=20;cube([width,20,10]);");auto plan=InsertionPlanner::geometry(*f.inventory,f.target(),f.fragment("scadmaxx.builtin:plain-hole",{{"diameter",4.0},{"depth",12.0}}),placed({100,100,10},true),false);REQUIRE(plan);auto response=f.preview(*plan.value);CHECK(response.status==PreviewStatus::Invalid);REQUIRE(!response.diagnostics.empty());CHECK(response.diagnostics.back().message.find("NoIntersection")!=std::string::npos);CHECK_FALSE(response.meshes);
  auto stale=f.inputs->root();stale.utf8="width=21;cube([width,20,10]);";CHECK_FALSE(InsertionPlanner::apply(stale,plan.value->patches));CHECK_FALSE(plan.value->patches.empty());
}
TEST_CASE("duplicate_start_insertions_coalesce_and_imported_identifiers_reserve_names","[insertion-source]"){
  Fixture f;f.write("library.scad","module smx_insert_item_1(){cube(1);}");f.evaluate("use <library.scad>\ncube(10);");std::set<std::string> reserved;CHECK(f.inventory->reserve("item",reserved)=="smx_insert_item_2");CHECK(f.inventory->reserve("item",reserved)=="smx_insert_item_3");auto combined=InsertionPlanner::coalesce(f.inputs->root(),{{{0,0,"first"},""},{{0,0,"second"},""}});REQUIRE(combined);REQUIRE(combined.value->size()==1);CHECK(combined.value->front().edit.replacement=="firstsecond");auto rejected=InsertionPlanner::coalesce(f.inputs->root(),{{{0,1,"x"},"u"},{{0,0,"y"},""}});CHECK_FALSE(rejected);
}
TEST_CASE("native_import_inventory_preserves_kind_order_and_helper_scope","[insertion-source]"){
  Fixture f;f.write("one.scad","module first(){}");f.write("two.scad","module second(){}");f.evaluate("include <one.scad>\nmodule helper(){include <two.scad> cube(1);}\nuse <two.scad>\ncube(10);");auto imports=f.inventory->imports(f.key());if(!imports)INFO(imports.diagnostic->message);REQUIRE(imports);REQUIRE(imports.value->size()==3);CHECK((*imports.value)[0].kind=="include");CHECK((*imports.value)[0].fileScope);CHECK_FALSE((*imports.value)[1].fileScope);CHECK((*imports.value)[2].kind=="use");CHECK((*imports.value)[2].fileScope);
}
TEST_CASE("required_file_imports_reuse_native_order_and_report_ambiguous_effects","[insertion-source]"){
  for(const auto& original:std::vector<std::string>{"include <std.scad>\ninclude <screws.scad>\ncube(10);","include <std.scad>\ncube(10);","include <screws.scad>\ninclude <std.scad>\ncube(10);","use <std.scad>\ninclude <screws.scad>\ncube(10);","include <std.scad>\ninclude <std.scad>\ninclude <screws.scad>\ncube(10);","cube(10);\ninclude <std.scad>\ninclude <screws.scad>\n"}){
    Fixture f;f.write("std.scad","module standard(){}");f.write("screws.scad","module screw(){}");f.evaluate(original);Providers::GeneratedFragment fragment;
    Providers::ResolvedLibrary library;library.libraryId="fixture";library.root=EvaluationInputs::fileKey(f.directory);fragment.binding.libraries.push_back(library);
    fragment.fileImports={{"fixture","std.scad",Providers::ImportRequest::Kind::Include,Providers::ImportRequest::Scope::File,{}},{"fixture","screws.scad",Providers::ImportRequest::Kind::Include,Providers::ImportRequest::Scope::File,{}}};
    auto prefix=f.inventory->filePrefixPoint(f.key());REQUIRE(prefix);auto planned=InsertionPlanner::imports(*f.inventory,f.key(),fragment,*prefix.value);
    if(original=="include <std.scad>\ninclude <screws.scad>\ncube(10);"){REQUIRE(planned);CHECK(planned.value->empty());}
    else if(original=="include <std.scad>\ncube(10);"){REQUIRE(planned);CHECK(*planned.value=="include <screws.scad>\n");}
    else {REQUIRE_FALSE(planned);CHECK(planned.diagnostic->code=="ImportConflict");}
  }
}
