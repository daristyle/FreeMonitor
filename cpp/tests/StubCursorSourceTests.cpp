#include "stub/StubCursorSource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

TEST_CASE("the stub cursor source says the platform has no cursor source") {
    fm::StubCursorSource source;
    const auto started = source.start(fm::CaptureTarget{.outputName = "FREEMONITOR-1"});
    REQUIRE_FALSE(started);
    CHECK(started.error().code == fm::ErrorCode::NOT_SUPPORTED);

    fm::CursorState state;
    CHECK(source.waitForChange(state, std::chrono::milliseconds{0}) == fm::CursorStatus::CURSOR_LOST);
    CHECK(source.image().pixels.empty());
}
