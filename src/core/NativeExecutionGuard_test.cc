#include "core/NativeExecutionGuard.h"
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <chrono>
TEST_CASE("Native parsing evaluation and geometry jobs share a recursive execution gate", "[evaluation-inputs][native-execution]") {
  std::promise<void> attempted;
  std::future<void> completed;
  auto *callerStack=&StackCheck::inst();
  bool workerHasOwnStack=false,workerRecursion=false;
  {
    NativeExecutionGuard outer;
    NativeExecutionGuard nested;
    completed = std::async(std::launch::async, [&] {
      attempted.set_value(); NativeExecutionGuard worker;
      workerHasOwnStack=&StackCheck::inst()!=callerStack;workerRecursion=StackCheck::inst().check();
    });
    attempted.get_future().wait();
    REQUIRE((completed.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout));
  }
  REQUIRE((completed.wait_for(std::chrono::seconds(1)) == std::future_status::ready)); completed.get();
  REQUIRE(workerHasOwnStack); REQUIRE_FALSE(workerRecursion);
}
