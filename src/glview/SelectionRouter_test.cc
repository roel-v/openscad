#include "glview/SelectionRouter.h"
#include <catch2/catch_test_macros.hpp>
TEST_CASE("Selection router replaces and cancels one exclusive owner exactly once", "[selection-router]")
{
  SelectionRouter router; int cancellations=0;
  auto first=router.begin(SelectionRouter::Mode::Pick,[&](auto reason) { REQUIRE(reason==SelectionRouter::CancelReason::Replaced); ++cancellations; });
  auto next=router.begin(SelectionRouter::Mode::Drag,[&](auto) { ++cancellations; });
  REQUIRE(next!=first); REQUIRE(cancellations==1);
  router.cancel(first,SelectionRouter::CancelReason::Escape); REQUIRE(cancellations==1);
  router.cancel(next,SelectionRouter::CancelReason::OwnerDestroyed); router.cancel(next,SelectionRouter::CancelReason::Escape);
  REQUIRE(cancellations==2); REQUIRE_FALSE(router.token());
}
TEST_CASE("Selection router keeps misses active and retires success and failures", "[selection-router]")
{
  SelectionRouter router; int cancellations=0;
  auto token=router.begin(SelectionRouter::Mode::Pick,[&](auto) { ++cancellations; });
  REQUIRE(router.accept(token,SelectionRouter::Outcome::Miss)); REQUIRE(router.token()==token);
  REQUIRE(router.accept(token,SelectionRouter::Outcome::Success)); REQUIRE_FALSE(router.token());
  REQUIRE_FALSE(router.accept(token,SelectionRouter::Outcome::Success)); REQUIRE(cancellations==0);
  token=router.begin(SelectionRouter::Mode::Pick,{}); REQUIRE(router.accept(token,SelectionRouter::Outcome::Failure)); REQUIRE_FALSE(router.token());
}
TEST_CASE("Selection router separates navigation drag from a pick click", "[selection-router]")
{
  SelectionRouter router; router.begin(SelectionRouter::Mode::Pick,{});
  router.press(10,10); router.move(12,11,10); REQUIRE(router.click());
  router.move(20,11,10); REQUIRE_FALSE(router.click());
  router.move(10,10,10); REQUIRE_FALSE(router.click());
  router.releasePress(); REQUIRE_FALSE(router.click());
}
