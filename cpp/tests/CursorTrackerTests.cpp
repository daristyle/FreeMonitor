#include "CursorTracker.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("a cursor that enters with a hotspot is visible at its position") {
    fm::CursorTracker tracker;
    tracker.enter();
    tracker.moveTo(674, 270);
    tracker.setHotspot(5, 1);

    fm::CursorState state;
    REQUIRE(tracker.takeChange(state));
    CHECK(state.visible);
    CHECK(state.x == 674);
    CHECK(state.y == 270);
    CHECK_FALSE(tracker.takeChange(state));
}

TEST_CASE("the cursor stays hidden until the compositor reports a hotspot") {
    fm::CursorTracker tracker;
    tracker.enter();
    tracker.moveTo(10, 20);

    fm::CursorState state;
    REQUIRE(tracker.takeChange(state));
    CHECK_FALSE(state.visible);
    CHECK(state.x == 10);
}

TEST_CASE("a hotspot of (0,0) means the compositor hid the cursor") {
    fm::CursorTracker tracker;
    fm::CursorState state;
    tracker.enter();
    tracker.setHotspot(5, 1);
    REQUIRE(tracker.takeChange(state));
    REQUIRE(state.visible);

    tracker.setHotspot(0, 0);
    REQUIRE(tracker.takeChange(state));
    CHECK_FALSE(state.visible);

    tracker.setHotspot(21, 12);
    REQUIRE(tracker.takeChange(state));
    CHECK(state.visible);
}

TEST_CASE("leaving the monitor hides the cursor and keeps its last position") {
    fm::CursorTracker tracker;
    fm::CursorState state;
    tracker.enter();
    tracker.setHotspot(5, 1);
    tracker.moveTo(1919, 500);
    REQUIRE(tracker.takeChange(state));

    tracker.leave();
    REQUIRE(tracker.takeChange(state));
    CHECK_FALSE(state.visible);
    CHECK(state.x == 1919);
    CHECK(state.y == 500);

    tracker.enter();
    REQUIRE(tracker.takeChange(state));
    CHECK(state.visible);
}

TEST_CASE("repeating the same values is not a change") {
    fm::CursorTracker tracker;
    fm::CursorState state;
    tracker.enter();
    tracker.setHotspot(5, 1);
    tracker.moveTo(3, 4);
    tracker.setImageId(1);
    REQUIRE(tracker.takeChange(state));

    tracker.enter();
    tracker.setHotspot(5, 1);
    tracker.moveTo(3, 4);
    tracker.setImageId(1);
    CHECK_FALSE(tracker.takeChange(state));
}

TEST_CASE("a new image is a change") {
    fm::CursorTracker tracker;
    fm::CursorState state;
    tracker.setImageId(2);
    REQUIRE(tracker.takeChange(state));
    CHECK(state.imageId == 2);
}
