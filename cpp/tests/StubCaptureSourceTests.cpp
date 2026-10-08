#include "stub/StubCaptureSource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using namespace std::chrono_literals;

namespace {
    const fm::CaptureTarget TEST_TARGET{"TEST-1"};

    void checkPixel(const fm::CapturedFrame& frame, std::uint32_t x, std::uint32_t y, std::uint64_t frameIndex) {
        const std::uint8_t* pixel = frame.pixels + static_cast<std::size_t>(y) * frame.stride +
                                    static_cast<std::size_t>(x) * fm::bytesPerPixel(frame.format);
        const auto expected = fm::StubCaptureSource::patternPixel(x, y, frameIndex);
        CHECK(pixel[0] == expected[0]);
        CHECK(pixel[1] == expected[1]);
        CHECK(pixel[2] == expected[2]);
        CHECK(pixel[3] == expected[3]);
    }
}

TEST_CASE("createCaptureSource returns a capture source") {
    CHECK(fm::createCaptureSource() != nullptr);
}

TEST_CASE("acquireFrame before start reports CAPTURE_LOST") {
    fm::StubCaptureSource captureSource;
    fm::CapturedFrame frame;
    CHECK(captureSource.acquireFrame(frame, 0ms) == fm::CaptureStatus::CAPTURE_LOST);
}

TEST_CASE("start rejects bad input and a second start") {
    SECTION("empty output name") {
        fm::StubCaptureSource captureSource;
        const auto result = captureSource.start(fm::CaptureTarget{});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    }
    SECTION("zero size") {
        fm::StubCaptureSource captureSource(0, 1080);
        const auto result = captureSource.start(TEST_TARGET);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    }
    SECTION("start twice") {
        fm::StubCaptureSource captureSource;
        REQUIRE(captureSource.start(TEST_TARGET));
        const auto result = captureSource.start(TEST_TARGET);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == fm::ErrorCode::INVALID_STATE);
    }
}

TEST_CASE("frames have the configured size, a padded stride and the test pattern") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    fm::StubCaptureSource captureSource(width, height);
    REQUIRE(captureSource.start(TEST_TARGET));

    fm::CapturedFrame frame;
    REQUIRE(captureSource.acquireFrame(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    REQUIRE(frame.pixels != nullptr);
    CHECK(frame.width == width);
    CHECK(frame.height == height);
    CHECK(frame.format == fm::StubCaptureSource::PIXEL_FORMAT);
    CHECK(frame.stride == width * fm::bytesPerPixel(frame.format) + fm::StubCaptureSource::ROW_PADDING_BYTES);
    checkPixel(frame, 0, 0, 0);
    checkPixel(frame, 17, 5, 0);
    checkPixel(frame, width - 1, height - 1, 0);
    captureSource.releaseFrame();

    const auto firstCaptureTime = frame.captureTime;
    REQUIRE(captureSource.acquireFrame(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    checkPixel(frame, 0, 0, 1);
    checkPixel(frame, width - 1, height - 1, 1);
    CHECK(frame.captureTime >= firstCaptureTime);
    captureSource.releaseFrame();
}

TEST_CASE("frames reuse the same buffer") {
    fm::StubCaptureSource captureSource(16, 16);
    REQUIRE(captureSource.start(TEST_TARGET));

    fm::CapturedFrame frame;
    REQUIRE(captureSource.acquireFrame(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    const std::uint8_t* firstPixels = frame.pixels;
    captureSource.releaseFrame();
    REQUIRE(captureSource.acquireFrame(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    CHECK(frame.pixels == firstPixels);
    captureSource.releaseFrame();
}

TEST_CASE("capture can be stopped and started again") {
    fm::StubCaptureSource captureSource(16, 16);
    REQUIRE(captureSource.start(TEST_TARGET));
    captureSource.stop();

    fm::CapturedFrame frame;
    CHECK(captureSource.acquireFrame(frame, 0ms) == fm::CaptureStatus::CAPTURE_LOST);

    REQUIRE(captureSource.start(TEST_TARGET));
    REQUIRE(captureSource.acquireFrame(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    checkPixel(frame, 3, 3, 0);
    captureSource.releaseFrame();
}

TEST_CASE("stop is safe when not started") {
    fm::StubCaptureSource captureSource;
    captureSource.stop();
    captureSource.stop();
}
