#include "JPEGEncoder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <turbojpeg.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

namespace {
    constexpr std::uint32_t FRAME_WIDTH = 64;
    constexpr std::uint32_t FRAME_HEIGHT = 32;
    constexpr std::uint32_t ROW_PADDING = 32;
    constexpr std::uint32_t PIXEL_SIZE = 4;
    constexpr std::uint32_t FRAME_STRIDE = FRAME_WIDTH * PIXEL_SIZE + ROW_PADDING;
    constexpr int COLOUR_TOLERANCE = 8;

    struct Colour {
        std::uint8_t red;
        std::uint8_t green;
        std::uint8_t blue;
    };

    struct TestImage {
        std::vector<std::uint8_t> pixels;
        fm::CapturedFrame frame;
    };

    TestImage makeImage(std::uint32_t width, std::uint32_t height, fm::PixelFormat format) {
        TestImage image;
        const std::uint32_t stride = width * PIXEL_SIZE + ROW_PADDING;
        image.pixels.assign(static_cast<std::size_t>(stride) * height, 0xEE);
        image.frame.pixels = image.pixels.data();
        image.frame.width = width;
        image.frame.height = height;
        image.frame.stride = stride;
        image.frame.format = format;
        image.frame.captureTime = std::chrono::steady_clock::now();
        return image;
    }

    TestImage makeSolidImage(fm::PixelFormat format, Colour colour) {
        TestImage image = makeImage(FRAME_WIDTH, FRAME_HEIGHT, format);
        const bool blueFirst = format == fm::PixelFormat::BGRA8;
        for (std::uint32_t y = 0; y < FRAME_HEIGHT; ++y) {
            for (std::uint32_t x = 0; x < FRAME_WIDTH; ++x) {
                std::uint8_t* pixel = image.pixels.data() + y * FRAME_STRIDE + x * PIXEL_SIZE;
                pixel[0] = blueFirst ? colour.blue : colour.red;
                pixel[1] = colour.green;
                pixel[2] = blueFirst ? colour.red : colour.blue;
                pixel[3] = 0xFF;
            }
        }
        return image;
    }

    TestImage makeNoiseImage() {
        TestImage image = makeImage(FRAME_WIDTH, FRAME_HEIGHT, fm::PixelFormat::BGRA8);
        std::minstd_rand random(1234);
        for (auto& byte : image.pixels) {
            byte = static_cast<std::uint8_t>(random());
        }
        return image;
    }

    fm::EncoderConfig makeConfig(fm::PixelFormat format = fm::PixelFormat::BGRA8) {
        return fm::EncoderConfig{.width = FRAME_WIDTH, .height = FRAME_HEIGHT, .inputFormat = format};
    }

    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgb;
    };

    DecodedImage decode(std::span<const std::uint8_t> jpeg) {
        DecodedImage decoded;
        tjhandle handle = ::tj3Init(TJINIT_DECOMPRESS);
        REQUIRE(handle != nullptr);
        REQUIRE(::tj3DecompressHeader(handle, jpeg.data(), jpeg.size()) == 0);
        decoded.width = ::tj3Get(handle, TJPARAM_JPEGWIDTH);
        decoded.height = ::tj3Get(handle, TJPARAM_JPEGHEIGHT);
        decoded.rgb.resize(static_cast<std::size_t>(decoded.width) * static_cast<std::size_t>(decoded.height) * 3);
        const int result = ::tj3Decompress8(handle, jpeg.data(), jpeg.size(), decoded.rgb.data(), 0, TJPF_RGB);
        ::tj3Destroy(handle);
        REQUIRE(result == 0);
        return decoded;
    }

    bool isClose(std::uint8_t actual, std::uint8_t expected) {
        return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= COLOUR_TOLERANCE;
    }
}

TEST_CASE("createEncoder returns the JPEG encoder for JPEG") {
    const auto encoder = fm::createEncoder(fm::EncoderChoice::JPEG);
    CHECK(dynamic_cast<fm::JPEGEncoder*>(encoder.get()) != nullptr);
    CHECK(encoder->description() == "JPEG");
}

TEST_CASE("JPEG configure rejects bad settings") {
    fm::JPEGEncoder encoder;
    auto config = makeConfig();

    SECTION("zero size") {
        config.width = 0;
        const auto result = encoder.configure(config);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    }
    SECTION("larger than JPEG allows") {
        config.width = 65501;
        CHECK_FALSE(encoder.configure(config));
    }
    SECTION("zero frame rate") {
        config.framesPerSecond = 0;
        CHECK_FALSE(encoder.configure(config));
    }
    SECTION("quality out of range") {
        config.jpegQuality = 0;
        CHECK_FALSE(encoder.configure(config));
        config.jpegQuality = 101;
        CHECK_FALSE(encoder.configure(config));
    }
}

TEST_CASE("JPEG encode fails without configure or with a mismatched frame") {
    auto image = makeSolidImage(fm::PixelFormat::BGRA8, Colour{255, 0, 0});
    fm::JPEGEncoder encoder;
    fm::EncodedFrame output;

    SECTION("not configured") {
        CHECK(encoder.encode(image.frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("wrong size") {
        REQUIRE(encoder.configure(makeConfig()));
        image.frame.height = FRAME_HEIGHT - 1;
        CHECK(encoder.encode(image.frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("wrong format") {
        REQUIRE(encoder.configure(makeConfig()));
        image.frame.format = fm::PixelFormat::RGBA8;
        CHECK(encoder.encode(image.frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("stride smaller than a row") {
        REQUIRE(encoder.configure(makeConfig()));
        image.frame.stride = FRAME_WIDTH * PIXEL_SIZE - 1;
        CHECK(encoder.encode(image.frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
    SECTION("no pixels") {
        REQUIRE(encoder.configure(makeConfig()));
        image.frame.pixels = nullptr;
        CHECK(encoder.encode(image.frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
}

TEST_CASE("JPEG encode produces a complete JPEG keyframe") {
    const auto image = makeSolidImage(fm::PixelFormat::BGRA8, Colour{20, 120, 220});
    fm::JPEGEncoder encoder;
    REQUIRE(encoder.configure(makeConfig()));

    fm::EncodedFrame output;
    REQUIRE(encoder.encode(image.frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    REQUIRE(output.data.size() >= 4);
    CHECK(output.data[0] == 0xFF);
    CHECK(output.data[1] == 0xD8);
    CHECK(output.data[output.data.size() - 2] == 0xFF);
    CHECK(output.data[output.data.size() - 1] == 0xD9);
    CHECK(output.data.size() < FRAME_WIDTH * FRAME_HEIGHT * PIXEL_SIZE);
    CHECK(output.codec == fm::Codec::JPEG);
    CHECK(output.keyframe);
    CHECK(output.captureTime == image.frame.captureTime);
}

TEST_CASE("JPEG keeps the colours of both pixel formats and skips row padding") {
    const Colour colour{230, 30, 90};
    const auto format = GENERATE(fm::PixelFormat::BGRA8, fm::PixelFormat::RGBA8);
    CAPTURE(format == fm::PixelFormat::BGRA8 ? "BGRA8" : "RGBA8");
    const auto image = makeSolidImage(format, colour);
    fm::JPEGEncoder encoder;
    REQUIRE(encoder.configure(makeConfig(format)));

    fm::EncodedFrame output;
    REQUIRE(encoder.encode(image.frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    const auto decoded = decode(output.data);
    REQUIRE(decoded.width == static_cast<int>(FRAME_WIDTH));
    REQUIRE(decoded.height == static_cast<int>(FRAME_HEIGHT));
    for (std::size_t i = 0; i < decoded.rgb.size(); i += 3) {
        CAPTURE(i / 3);
        REQUIRE(isClose(decoded.rgb[i], colour.red));
        REQUIRE(isClose(decoded.rgb[i + 1], colour.green));
        REQUIRE(isClose(decoded.rgb[i + 2], colour.blue));
    }
}

TEST_CASE("JPEG encode reuses its output buffer") {
    const auto image = makeSolidImage(fm::PixelFormat::BGRA8, Colour{0, 255, 0});
    fm::JPEGEncoder encoder;
    REQUIRE(encoder.configure(makeConfig()));

    fm::EncodedFrame output;
    REQUIRE(encoder.encode(image.frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    const std::uint8_t* firstData = output.data.data();
    REQUIRE(encoder.encode(image.frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    CHECK(output.data.data() == firstData);
}

TEST_CASE("JPEG reconfigure changes the frame size") {
    fm::JPEGEncoder encoder;
    REQUIRE(encoder.configure(makeConfig()));

    constexpr std::uint32_t newWidth = 40;
    constexpr std::uint32_t newHeight = 24;
    const auto image = makeImage(newWidth, newHeight, fm::PixelFormat::BGRA8);
    REQUIRE(encoder.configure(fm::EncoderConfig{.width = newWidth, .height = newHeight}));

    fm::EncodedFrame output;
    REQUIRE(encoder.encode(image.frame, output) == fm::EncodeStatus::FRAME_ENCODED);
    const auto decoded = decode(output.data);
    CHECK(decoded.width == static_cast<int>(newWidth));
    CHECK(decoded.height == static_cast<int>(newHeight));
}

TEST_CASE("JPEG lowers the quality when a frame is over the size limit") {
    const auto image = makeNoiseImage();
    auto config = makeConfig();
    config.jpegQuality = 100;

    fm::JPEGEncoder unlimitedEncoder;
    REQUIRE(unlimitedEncoder.configure(config));
    fm::EncodedFrame unlimitedOutput;
    REQUIRE(unlimitedEncoder.encode(image.frame, unlimitedOutput) == fm::EncodeStatus::FRAME_ENCODED);
    const std::size_t fullQualitySize = unlimitedOutput.data.size();

    SECTION("a lower quality fits") {
        const std::size_t limit = fullQualitySize - 1;
        fm::JPEGEncoder encoder(limit);
        REQUIRE(encoder.configure(config));
        fm::EncodedFrame output;
        REQUIRE(encoder.encode(image.frame, output) == fm::EncodeStatus::FRAME_ENCODED);
        CHECK(output.data.size() <= limit);
        const auto decoded = decode(output.data);
        CHECK(decoded.width == static_cast<int>(FRAME_WIDTH));
    }
    SECTION("nothing fits") {
        fm::JPEGEncoder encoder(1);
        REQUIRE(encoder.configure(config));
        fm::EncodedFrame output;
        CHECK(encoder.encode(image.frame, output) == fm::EncodeStatus::ENCODE_FAILED);
    }
}
