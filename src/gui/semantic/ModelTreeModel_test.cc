#include "gui/semantic/ModelTreeModel.h"
#include "gui/semantic/SemanticMeshJob.h"
#include "core/semantic/SemanticTestFixture.h"
TEST_CASE("Semantic model tree has exact hierarchy definitions ordinary and residual rows", "[semantic-tree]") {
  SemanticTest::Fixture fixture;fixture.evaluate(SemanticTest::pair+"translate([200,0,0]) cube(2);");
  Semantic::ModelTreeModel model;model.setScene(fixture.scene,"result");REQUIRE(model.rowCount()==2);
  auto pair=model.find(Semantic::TreeRole::Instance,"/pair");auto right=model.find(Semantic::TreeRole::Instance,"/pair/right");REQUIRE(pair.isValid());REQUIRE(model.parent(right)==pair);
  REQUIRE(model.find(Semantic::TreeRole::Other,"").isValid());REQUIRE(model.find(Semantic::TreeRole::Definition,"bracket").isValid());
  REQUIRE(right.data(Semantic::ModelTreeModel::ResultRole).toString()=="result");REQUIRE_FALSE(model.flags(right)&Qt::ItemIsUserCheckable);
  Semantic::Visibility state;state.hidden.insert("/pair");model.setVisibility(state,true);REQUIRE(right.data(Qt::CheckStateRole)==Qt::Unchecked);REQUIRE(model.flags(right)&Qt::ItemIsUserCheckable);
  SemanticTest::Fixture ordinary;ordinary.evaluate("cube(1);");model.setScene(ordinary.scene,"ordinary");REQUIRE(model.rowCount()==1);REQUIRE(model.rowCount(model.index(0,0))==0);
  model.setScene({},"", "Source changed");REQUIRE(model.index(0,0).data().toString()=="Source changed");
}
TEST_CASE("Semantic inspection meshes preserve finished Part parameterization placement roles and color", "[semantic-tree][semantic-visibility]") {
  SemanticTest::Fixture fixture;fixture.evaluate(SemanticTest::pair+"color([1,0,0]) translate([200,0,0]) @{type:Component,id:\"red\"} bracket(20); %translate([250,0,0]) @{type:Component,id:\"background\"} bracket(); cube(2);");
  auto output=Semantic::prepareMeshes(*fixture.scene,"result",1);INFO(output.error);REQUIRE(output.meshes);REQUIRE(output.meshes->pieces.size()==5);
  auto find=[&](std::string path)->const InspectionPiece& {for(const auto& piece:output.meshes->pieces)if(output.paths.at(piece.drawId)==path)return piece;FAIL("Missing path");throw std::runtime_error("missing");};
  REQUIRE(find("/pair/right").mesh->getBoundingBox().max().x()==45);REQUIRE(find("/pair/left").mesh->getBoundingBox().max().x()==35);
  REQUIRE(find("/pair/right").localToWorld.translation().x()==60);REQUIRE(find("/background").role==InspectionPiece::Role::Background);
  REQUIRE(find("/red").mesh->colors.size()==1);REQUIRE(find("/red").mesh->colors.front().isValid());
  REQUIRE(Semantic::prepareMeshes(*fixture.scene,"result",2,[]{return true;}).cancelled);
  SemanticTest::Fixture flat;flat.evaluate("@{type:Part,id:\"flat\"} module flat(){square(2);} @{type:Component,id:\"flat\"} flat();");
  auto unsupported=Semantic::prepareMeshes(*flat.scene,"flat",1);REQUIRE_FALSE(unsupported.meshes);REQUIRE_FALSE(unsupported.error.empty());
}
