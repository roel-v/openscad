#include "core/ParserInspection.h"
#include "core/SourceFile.h"
#include "openscad.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Native inspection budgets remain bounded and permit explicitly larger captured inventories","[native-inspection-budget]"){
  std::string source;for(size_t i=0;i<3000;++i)source+="value_"+std::to_string(i)+"="+std::to_string(i)+";\n";source+="cube(1);\n";
  ParserInspection small;small.read=[](const std::string&)->std::string{throw std::runtime_error("Unexpected inspection source read");};small.dependency=[](const auto&,const auto&,const auto&,const auto&){};small.alive=[](){return true;};SourceFile *raw=nullptr;
  CHECK_FALSE(parseInspection(raw,source,"inspection-budget.scad",small));std::unique_ptr<SourceFile> rejected(raw);CHECK(small.failed);CHECK(small.tokens>8192);CHECK(parserInspection==nullptr);
  ParserInspection large=small;large.tokens=0;large.nesting=0;large.failed=false;large.tokenLimit=32768;raw=nullptr;REQUIRE(parseInspection(raw,source,"inspection-budget.scad",large));std::unique_ptr<SourceFile> accepted(raw);REQUIRE(accepted);CHECK(accepted->scope->assignments.size()==3000);CHECK_FALSE(large.failed);CHECK(large.tokens<=large.tokenLimit);CHECK(parserInspection==nullptr);
}
