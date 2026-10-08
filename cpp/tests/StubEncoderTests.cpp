#include "stub/StubCaptureSource.hpp"
#include "stub/StubEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::uint32_t FRAME_WIDTH = 3;
    constexpr std::uint32_t FRAME_HEIGHT = 2;
    constexpr fm::PixelFormat FRAME_FORMAT = fm::PixelFormat::BGRA8;
    constexpr std::uint32_t PIXEL_SIZE = fm::bytesPerPixel(FRAME_FORMAT);
    constexpr std::uint32_t ROW_BYTES = FRAME_WIDTH * PIXEL_SIZE;
    constexpr std::uint32_t FRAME_STRIDE = 16;

    std::vector<std::uint8_t> makePaddedPixels() {
        std::vector<std::uint8_t> pixels(FRAME_STRIDE * FRAME_HEIGHT, 0xEE);
        for (std::uint32_t y = 0; y < FRAME_HEIGHT; ++y) {
            for (std::uint32_t i = 0; i < ROW_BYTES; ++i) {
                pixels[y * FRAME_STRIDE + i] = static_cast<std::uint8_t>(y * 100 + i);
            }
        }
        return pixels;
    }

    fm::CapturedFrame makeFrame(const std::vector<std::uint8_t>& pixels) {
        fm::CapturedFrame frame;
        frame.pixels = pixels.data();
        frame.width = FRAME_WIDTH;
        frame.height = FRAME_HEIGHT;
        frame.stride = FRAME_STRIDE;
        frame.format = FRAME_FORMAT;
        frame.captureTime = std::chrono::steady_clock::now();
        return frame;
    }

    fm::EncoderConfig makeConfig() {
        return fm::EncoderConfig{.width = FRAME_WIDTH, .height = FRAME_HEIGHT, .inputFormat = FRAME_FORMAT};
    }
}

TEST_CASE("createEncoder returns an encoder for every codec") {
    CHECK(fm::createEncoder(fm::Codec::JPEG) != nullptr);
    CHECK(fm::createEncoder(fm::Codec::H264) != nullptr);
}

TEST_CASE("configure rejects bad settings") {
    SECTION("zero size") {
        fm::StubEncoder encoder(fm::Codec::H264);
        auto config = makeConfig();
        config.height = 0;
        const auto result = encoder.configure(config);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    }
    SECTION("zero frame rate") {
        fm::StubEncoder encoder(fm::Codec::H264);
        auto config = makeConfig();
        config.framesPerSecond = 0;
        CHECK_FALSE(encoder.configure(config));
    }
    SECTION("JPEG quality out of range") {
        fm::StubEncoder encoder(fm::Codec::JPEG);
        auto config = makeConfig();
        config.jpegQuality = 0;
        CHECK_FALSE(encoder.configure(config));
        config.jpegQuality = 101;
        CHECK_FALSE(encoder.configure(config));
    }
}

TEST_CASE("encode fails without configure or with a mismatched frame") {
    const auto pixels = makePaddedPixels();
    auto frame = makeFrame(pixels);
    fm::EncodedFrame output;

    SECTION("not configured") {
        fm::StubEncoder encoder(fm::Codec::JPEG);
        CHECK(encoder.encode(frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("wrong size") {
        fm::StubEncoder encoder(fm::Codec::JPEG);
        REQUIRE(encoder.configure(makeConfig()));
        frame.width = FRAME_WIDTH + 1;
        CHECK(encoder.encode(frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("wrong format") {
        fm::StubEncoder encoder(fm::Codec::JPEG);
        REQUIRE(encoder.configure(makeConfig()));
        frame.format = fm::PixelFormat::RGBA8;
        CHECK(encoder.encode(frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("stride smaller than a row") {
        fm::StubEncoder encoder(fm::Codec::JPEG);
        REQUIRE(encoder.configure(makeConfig()));
        frame.stride = ROW_BYTES - 1;
        CHECK(encoder.encode(frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("no pixels") {
        fm::StubEncoder encoder(fm::Codec::JPEG);
        REQUIRE(encoder.configure(makeConfig()));
        frame.pixels = nullptr;
        CHECK(encoder.encode(frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
}

TEST_CASE("encode removes the row padding and keeps the capture time") {
    const auto pixels = makePaddedPixels();
    const auto frame = makeFrame(pixels);
    fm::StubEncoder encoder(fm::Codec::JPEG);
    REQUIRE(encoder.configure(makeConfig()));

    fm::EncodedFrame output;
    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    REQUIRE(output.data.size() == ROW_BYTES * FRAME_HEIGHT);
    for (std::uint32_t y = 0; y < FRAME_HEIGHT; ++y) {
        for (std::uint32_t i = 0; i < ROW_BYTES; ++i) {
            CHECK(output.data[y * ROW_BYTES + i] == pixels[y * FRAME_STRIDE + i]);
        }
    }
    CHECK(output.captureTime == frame.captureTime);
}

TEST_CASE("encode reuses its output buffer") {
    const auto pixels = makePaddedPixels();
    const auto frame = makeFrame(pixels);
    fm::StubEncoder encoder(fm::Codec::JPEG);
    REQUIRE(encoder.configure(makeConfig()));

    fm::EncodedFrame output;
    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    const std::uint8_t* firstData = output.data.data();
    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    CHECK(output.data.data() == firstData);
}

TEST_CASE("every JPEG frame is a keyframe") {
    const auto pixels = makePaddedPixels();
    const auto frame = makeFrame(pixels);
    fm::StubEncoder encoder(fm::Codec::JPEG);
    REQUIRE(encoder.configure(makeConfig()));

    fm::EncodedFrame output;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
        CHECK(output.keyframe);
    }
}

TEST_CASE("H.264 keyframes come first, on request and after reconfigure") {
    const auto pixels = makePaddedPixels();
    const auto frame = makeFrame(pixels);
    fm::StubEncoder encoder(fm::Codec::H264);
    REQUIRE(encoder.configure(makeConfig()));
    fm::EncodedFrame output;

    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    CHECK(output.keyframe);
    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    CHECK_FALSE(output.keyframe);

    encoder.requestKeyframe();
    encoder.requestKeyframe();
    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    CHECK(output.keyframe);
    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    CHECK_FALSE(output.keyframe);

    REQUIRE(encoder.configure(makeConfig()));
    REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    CHECK(output.keyframe);
}

TEST_CASE("encoder accepts frames from the stub capture source") {
    constexpr std::uint32_t width = 40;
    constexpr std::uint32_t height = 20;
    fm::StubCaptureSource captureSource(width, height);
    REQUIRE(captureSource.start(fm::CaptureTarget{"TEST-1"}));
    fm::StubEncoder encoder(fm::Codec::JPEG);
    REQUIRE(encoder.configure(fm::EncoderConfig{.width = width, .height = height}));

    fm::CapturedFrame capturedFrame;
    fm::EncodedFrame encodedFrame;
    REQUIRE(captureSource.acquireFrame(capturedFrame, 0ms) == fm::CaptureStatus::FRAME_READY);
    REQUIRE(encoder.encode(capturedFrame, encodedFrame) == fm::EncodeStatus::FRAME_ENCODED);
    captureSource.releaseFrame();

    REQUIRE(encodedFrame.data.size() == width * height * PIXEL_SIZE);
    const auto lastPixel = fm::StubCaptureSource::patternPixel(width - 1, height - 1, 0);
    const std::size_t lastPixelOffset = (static_cast<std::size_t>(width) * height - 1) * PIXEL_SIZE;
    for (std::size_t i = 0; i < PIXEL_SIZE; ++i) {
        CHECK(encodedFrame.data[lastPixelOffset + i] == lastPixel[i]);
    }
}
