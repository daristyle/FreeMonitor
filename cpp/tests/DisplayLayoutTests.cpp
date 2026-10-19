#include "DisplayLayout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {
    const std::vector<fm::MonitorLayout> LAPTOP_ONLY{{.x = 0, .y = 0, .width = 1920, .height = 1080}};

    const std::vector<fm::MonitorLayout> THREE_MONITORS{
            {.x = 0, .y = 0, .width = 1920, .height = 1080},
            {.x = 1920, .y = -300, .width = 1280, .height = 800},
            {.x = -1080, .y = -200, .width = 1080, .height = 1920},
    };

    fm::DisplayMode modeWith(fm::DisplayPlacement placement) {
        return fm::DisplayMode{.width = 2560, .height = 1600, .refreshRateHz = 60, .placement = placement};
    }
}

TEST_CASE("placeDisplay puts the display next to a single monitor") {
    CHECK(fm::placeDisplay(LAPTOP_ONLY, modeWith(fm::DisplayPlacement::RIGHT)) == fm::DisplayPosition{1920, 0});
    CHECK(fm::placeDisplay(LAPTOP_ONLY, modeWith(fm::DisplayPlacement::LEFT)) == fm::DisplayPosition{-2560, 0});
    CHECK(fm::placeDisplay(LAPTOP_ONLY, modeWith(fm::DisplayPlacement::ABOVE)) == fm::DisplayPosition{0, -1600});
    CHECK(fm::placeDisplay(LAPTOP_ONLY, modeWith(fm::DisplayPlacement::BELOW)) == fm::DisplayPosition{0, 1080});
}

TEST_CASE("placeDisplay goes past the outermost monitor and lines up with it") {
    CHECK(fm::placeDisplay(THREE_MONITORS, modeWith(fm::DisplayPlacement::RIGHT)) == fm::DisplayPosition{3200, -300});
    CHECK(fm::placeDisplay(THREE_MONITORS, modeWith(fm::DisplayPlacement::LEFT)) ==
          fm::DisplayPosition{-1080 - 2560, -200});
    CHECK(fm::placeDisplay(THREE_MONITORS, modeWith(fm::DisplayPlacement::ABOVE)) ==
          fm::DisplayPosition{1920, -300 - 1600});
    CHECK(fm::placeDisplay(THREE_MONITORS, modeWith(fm::DisplayPlacement::BELOW)) == fm::DisplayPosition{-1080, 1720});
}

TEST_CASE("placeDisplay uses an explicit position as given") {
    auto mode = modeWith(fm::DisplayPlacement::EXPLICIT);
    mode.positionX = -2560;
    mode.positionY = 40;
    CHECK(fm::placeDisplay(THREE_MONITORS, mode) == fm::DisplayPosition{-2560, 40});
    CHECK(fm::placeDisplay({}, mode) == fm::DisplayPosition{-2560, 40});
}

TEST_CASE("placeDisplay starts at the origin when there are no monitors") {
    CHECK(fm::placeDisplay({}, modeWith(fm::DisplayPlacement::LEFT)) == fm::DisplayPosition{0, 0});
}
