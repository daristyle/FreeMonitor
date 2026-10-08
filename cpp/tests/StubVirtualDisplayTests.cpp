#include "stub/StubCaptureSource.hpp"
#include "stub/StubVirtualDisplay.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {
    constexpr fm::DisplayMode TEST_MODE{
            .width = 2560, .height = 1600, .refreshRateHz = 60, .positionX = 1920, .positionY = 0};
}

TEST_CASE("createVirtualDisplay returns a virtual display") {
    CHECK(fm::createVirtualDisplay() != nullptr);
}

TEST_CASE("create returns the output to capture and keeps the mode") {
    fm::StubVirtualDisplay virtualDisplay;
    const auto target = virtualDisplay.create(TEST_MODE);
    REQUIRE(target);
    CHECK(target->outputName == fm::StubVirtualDisplay::OUTPUT_NAME);
    CHECK(virtualDisplay.exists());
    CHECK(virtualDisplay.currentMode().width == TEST_MODE.width);
    CHECK(virtualDisplay.currentMode().height == TEST_MODE.height);
    CHECK(virtualDisplay.currentMode().positionX == TEST_MODE.positionX);
}

TEST_CASE("create rejects a bad mode") {
    fm::StubVirtualDisplay virtualDisplay;
    auto mode = TEST_MODE;

    SECTION("zero width") {
        mode.width = 0;
    }
    SECTION("zero height") {
        mode.height = 0;
    }
    SECTION("zero refresh rate") {
        mode.refreshRateHz = 0;
    }

    const auto result = virtualDisplay.create(mode);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    CHECK_FALSE(virtualDisplay.exists());
}

TEST_CASE("create twice is INVALID_STATE until the display is destroyed") {
    fm::StubVirtualDisplay virtualDisplay;
    REQUIRE(virtualDisplay.create(TEST_MODE));

    const auto second = virtualDisplay.create(TEST_MODE);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == fm::ErrorCode::INVALID_STATE);

    virtualDisplay.destroy();
    CHECK_FALSE(virtualDisplay.exists());
    CHECK(virtualDisplay.create(TEST_MODE));
}

TEST_CASE("destroy is safe when no display exists") {
    fm::StubVirtualDisplay virtualDisplay;
    virtualDisplay.destroy();
    virtualDisplay.destroy();
    CHECK_FALSE(virtualDisplay.exists());
}

TEST_CASE("the returned target can start a capture") {
    fm::StubVirtualDisplay virtualDisplay;
    const auto target = virtualDisplay.create(TEST_MODE);
    REQUIRE(target);

    fm::StubCaptureSource captureSource(TEST_MODE.width, TEST_MODE.height);
    CHECK(captureSource.start(*target));
}
