#include "FallbackEncoder.hpp"
#include "H264NALUnits.hpp"
#include "NVENCEncoder.hpp"
#include "linux/VAAPIEncoder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {
    constexpr std::uint32_t FRAME_WIDTH = 640;
    constexpr std::uint32_t FRAME_HEIGHT = 360;
    constexpr std::uint32_t PIXEL_SIZE = 4;
    constexpr std::uint32_t ROW_PADDING = 64;
    constexpr std::uint32_t BOX_SIZE = 32;
    constexpr std::uint32_t BOX_STEP = 8;
    constexpr int MOVING_FRAME_COUNT = 30;
    constexpr int STATIC_FRAME_COUNT = 10;
    constexpr std::size_t STATIC_BUDGET_FRACTION = 100;
    constexpr int SCENE_CHANGE_COUNT = 10;
    constexpr std::uint32_t OVERSIZED_FRAME_SIZE = 8192;
    constexpr int JUMPING_FRAME_COUNT = 9;
    constexpr std::uint32_t JUMPING_BOX_STEP = BOX_SIZE * 2;
    constexpr std::size_t MIN_NOISE_TO_KEYFRAME_RATIO = 2;
    constexpr std::size_t BITS_PER_KILOBIT = 1000;
    constexpr std::size_t BITS_PER_BYTE = 8;
    constexpr int COLOUR_TOLERANCE = 24;
    constexpr std::uint8_t OPAQUE = 255;

    struct Colour {
        std::uint8_t red;
        std::uint8_t green;
        std::uint8_t blue;
    };

    constexpr Colour RED{220, 40, 40};
    constexpr Colour GREEN{40, 200, 60};
    constexpr Colour BLUE{40, 60, 220};
    constexpr Colour WHITE{230, 230, 230};
    constexpr Colour BLACK{16, 16, 16};

    struct TestImage {
        std::vector<std::uint8_t> pixels;
        fm::CapturedFrame frame;
    };

    Colour quadrantColour(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
        const bool right = x >= width / 2;
        const bool bottom = y >= height / 2;
        if (!bottom) {
            return right ? GREEN : RED;
        }
        return right ? WHITE : BLUE;
    }

    TestImage makeImage(fm::PixelFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t boxX = 0) {
        const std::uint32_t stride = width * PIXEL_SIZE + ROW_PADDING;
        TestImage image{std::vector<std::uint8_t>(static_cast<std::size_t>(stride) * height), {}};
        const std::uint32_t boxTop = height / 2 - BOX_SIZE / 2;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const bool inBox = x >= boxX && x < boxX + BOX_SIZE && y >= boxTop && y < boxTop + BOX_SIZE;
                const Colour colour = inBox ? BLACK : quadrantColour(x, y, width, height);
                std::uint8_t* pixel = image.pixels.data() + static_cast<std::size_t>(y) * stride + x * PIXEL_SIZE;
                const bool bgra = format == fm::PixelFormat::BGRA8;
                pixel[0] = bgra ? colour.blue : colour.red;
                pixel[1] = colour.green;
                pixel[2] = bgra ? colour.red : colour.blue;
                pixel[3] = OPAQUE;
            }
        }
        image.frame = fm::CapturedFrame{.pixels = image.pixels.data(),
                                        .width = width,
                                        .height = height,
                                        .stride = stride,
                                        .format = format,
                                        .captureTime = std::chrono::steady_clock::now()};
        return image;
    }

    TestImage makeNoiseImage() {
        const std::uint32_t stride = FRAME_WIDTH * PIXEL_SIZE + ROW_PADDING;
        TestImage image{std::vector<std::uint8_t>(static_cast<std::size_t>(stride) * FRAME_HEIGHT), {}};
        std::mt19937 generator;
        for (std::uint8_t& byte : image.pixels) {
            byte = static_cast<std::uint8_t>(generator());
        }
        image.frame = fm::CapturedFrame{.pixels = image.pixels.data(),
                                        .width = FRAME_WIDTH,
                                        .height = FRAME_HEIGHT,
                                        .stride = stride,
                                        .format = fm::PixelFormat::BGRA8,
                                        .captureTime = std::chrono::steady_clock::now()};
        return image;
    }

    fm::EncoderConfig makeConfig(fm::PixelFormat format = fm::PixelFormat::BGRA8, std::uint32_t width = FRAME_WIDTH,
                                 std::uint32_t height = FRAME_HEIGHT) {
        return fm::EncoderConfig{.width = width, .height = height, .inputFormat = format};
    }

    enum class HardwareEncoder : std::uint8_t {
        NVENC,
        VAAPI,
    };

    const char* nameOf(HardwareEncoder kind) {
        return kind == HardwareEncoder::NVENC ? "NVENC" : "VA-API";
    }

    std::unique_ptr<fm::Encoder> makeEncoder(HardwareEncoder kind, std::size_t maxFrameSize = fm::MAX_FRAME_SIZE) {
        if (kind == HardwareEncoder::NVENC) {
            return std::make_unique<fm::NVENCEncoder>(maxFrameSize);
        }
        const auto devices = fm::findVAAPIDevices();
        return std::make_unique<fm::VAAPIEncoder>(devices.empty() ? fm::VAAPIDevice{} : devices.front(), maxFrameSize);
    }

    void configureOrSkip(fm::Encoder& encoder, const fm::EncoderConfig& config, HardwareEncoder kind) {
        const auto configured = encoder.configure(config);
        if (!configured) {
            SKIP(nameOf(kind) << " is not available: " << configured.error().message);
        }
    }

    std::vector<std::uint8_t> encodeCopy(fm::Encoder& encoder, const fm::CapturedFrame& frame, bool& keyframe) {
        fm::EncodedFrame output;
        REQUIRE(encoder.encode(frame, output) == fm::EncodeStatus::FRAME_ENCODED);
        CHECK(output.codec == fm::Codec::H264);
        CHECK(output.captureTime == frame.captureTime);
        keyframe = output.keyframe;
        return {output.data.begin(), output.data.end()};
    }

    bool isIDRWithParameterSets(std::span<const std::uint8_t> accessUnit) {
        const auto types = nalUnitTypes(accessUnit);
        return startsWithStartCode(accessUnit) && containsNALUnit(types, H264_SPS) &&
               containsNALUnit(types, H264_PPS) && containsNALUnit(types, H264_IDR_SLICE);
    }

    bool isDeltaFrame(std::span<const std::uint8_t> accessUnit) {
        const auto types = nalUnitTypes(accessUnit);
        return startsWithStartCode(accessUnit) && containsNALUnit(types, H264_NON_IDR_SLICE) &&
               !containsNALUnit(types, H264_IDR_SLICE);
    }

    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> luma{};
        std::vector<std::uint8_t> blueDifference{};
        std::vector<std::uint8_t> redDifference{};
        int lumaStride = 0;
        int chromaStride = 0;
        H264ColourDescription colours{};

        Colour colourAt(int x, int y) const {
            const bool bt709 = colours.matrix == H264_MATRIX_BT709;
            const double kr = bt709 ? 0.2126 : 0.299;
            const double kb = bt709 ? 0.0722 : 0.114;
            const double lumaScale = colours.fullRange ? 1.0 : 255.0 / 219.0;
            const double chromaScale = colours.fullRange ? 1.0 : 255.0 / 224.0;
            const double lumaOffset = colours.fullRange ? 0.0 : 16.0;
            const auto chromaIndex = static_cast<std::size_t>((y / 2) * chromaStride + x / 2);
            const double yValue = (luma[static_cast<std::size_t>(y * lumaStride + x)] - lumaOffset) * lumaScale;
            const double pb = (blueDifference[chromaIndex] - 128.0) * chromaScale;
            const double pr = (redDifference[chromaIndex] - 128.0) * chromaScale;
            const double red = yValue + 2.0 * (1.0 - kr) * pr;
            const double blue = yValue + 2.0 * (1.0 - kb) * pb;
            const double green = (yValue - kr * red - kb * blue) / (1.0 - kr - kb);
            const auto clamp = [](double value) {
                return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            };
            return Colour{clamp(red), clamp(green), clamp(blue)};
        }
    };

    class TestDecoder {
    private:
        struct ContextDeleter {
            void operator()(AVCodecContext* context) const noexcept {
                ::avcodec_free_context(&context);
            }
        };

        struct FrameDeleter {
            void operator()(AVFrame* frame) const noexcept {
                ::av_frame_free(&frame);
            }
        };

        struct PacketDeleter {
            void operator()(AVPacket* packet) const noexcept {
                ::av_packet_free(&packet);
            }
        };

        std::unique_ptr<AVCodecContext, ContextDeleter> context;
        std::unique_ptr<AVFrame, FrameDeleter> frame{::av_frame_alloc()};
        std::unique_ptr<AVPacket, PacketDeleter> packet{::av_packet_alloc()};

    public:
        bool open() {
            const AVCodec* codec = ::avcodec_find_decoder_by_name("libopenh264");
            if (codec == nullptr || !frame || !packet) {
                return false;
            }
            context.reset(::avcodec_alloc_context3(codec));
            return context && ::avcodec_open2(context.get(), codec, nullptr) == 0;
        }

        std::optional<DecodedImage> decode(std::span<const std::uint8_t> accessUnit) {
            packet->data = const_cast<std::uint8_t*>(accessUnit.data());
            packet->size = static_cast<int>(accessUnit.size());
            if (::avcodec_send_packet(context.get(), packet.get()) < 0 ||
                ::avcodec_send_packet(context.get(), nullptr) < 0 ||
                ::avcodec_receive_frame(context.get(), frame.get()) < 0 || frame->format != AV_PIX_FMT_YUV420P) {
                return std::nullopt;
            }
            DecodedImage image{.width = frame->width,
                               .height = frame->height,
                               .lumaStride = frame->linesize[0],
                               .chromaStride = frame->linesize[1],
                               .colours = spsColourDescription(accessUnit).value_or(H264ColourDescription{})};
            const auto lumaSize = static_cast<std::size_t>(frame->linesize[0] * frame->height);
            const auto chromaSize = static_cast<std::size_t>(frame->linesize[1] * ((frame->height + 1) / 2));
            image.luma.assign(frame->data[0], frame->data[0] + lumaSize);
            image.blueDifference.assign(frame->data[1], frame->data[1] + chromaSize);
            image.redDifference.assign(frame->data[2], frame->data[2] + chromaSize);
            return image;
        }
    };

    void openOrSkip(TestDecoder& decoder) {
        if (!decoder.open()) {
            SKIP("The libopenh264 decoder is not available");
        }
    }

    bool isClose(std::uint8_t actual, std::uint8_t expected) {
        return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= COLOUR_TOLERANCE;
    }

    void checkColour(const DecodedImage& image, int x, int y, Colour expected) {
        const Colour actual = image.colourAt(x, y);
        const int actualRed = actual.red;
        const int actualGreen = actual.green;
        const int actualBlue = actual.blue;
        const int matrix = image.colours.matrix;
        CAPTURE(x, y, actualRed, actualGreen, actualBlue, matrix, image.colours.fullRange);
        CHECK(image.colours.present);
        CHECK((matrix == H264_MATRIX_BT709 || matrix == H264_MATRIX_BT470BG || matrix == H264_MATRIX_SMPTE170M));
        CHECK(isClose(actual.red, expected.red));
        CHECK(isClose(actual.green, expected.green));
        CHECK(isClose(actual.blue, expected.blue));
    }
}

TEST_CASE("createEncoder returns NVENC for nvenc, VA-API for vaapi and a fallback chain for auto") {
    const auto nvenc = fm::createEncoder(fm::EncoderChoice::NVENC);
    CHECK(dynamic_cast<fm::NVENCEncoder*>(nvenc.get()) != nullptr);
    CHECK(nvenc->description() == "H.264 NVENC");
    const auto vaapi = fm::createEncoder(fm::EncoderChoice::VAAPI);
    CHECK(dynamic_cast<fm::VAAPIEncoder*>(vaapi.get()) != nullptr);
    CHECK(vaapi->description().starts_with("H.264 VA-API"));
    CHECK(dynamic_cast<fm::FallbackEncoder*>(fm::createEncoder(fm::EncoderChoice::AUTO).get()) != nullptr);
}

TEST_CASE("hardware encoders reject bad settings in configure") {
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    auto config = makeConfig();
    SECTION("too small") {
        config.width = 1;
    }
    SECTION("zero frame rate") {
        config.framesPerSecond = 0;
    }
    SECTION("zero bitrate") {
        config.bitrateKbps = 0;
    }
    const auto configured = encoder->configure(config);
    REQUIRE_FALSE(configured);
    CHECK(configured.error().code == fm::ErrorCode::INVALID_ARGUMENT);
}

TEST_CASE("a failed hardware encoder configure carries FFmpeg's reason in its error message") {
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(), kind);
    const auto configured =
            encoder->configure(makeConfig(fm::PixelFormat::BGRA8, OVERSIZED_FRAME_SIZE, OVERSIZED_FRAME_SIZE));
    REQUIRE_FALSE(configured);
    CAPTURE(configured.error().message);
    CHECK(configured.error().message.find("(FFmpeg: ") != std::string::npos);
}

TEST_CASE("hardware encode fails without configure or with a mismatched frame") {
    const auto image = makeImage(fm::PixelFormat::BGRA8, FRAME_WIDTH, FRAME_HEIGHT);
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    fm::EncodedFrame output;
    CHECK(encoder->encode(image.frame, output) == fm::EncodeStatus::ENCODE_FAILED);

    configureOrSkip(*encoder, makeConfig(), kind);
    auto frame = image.frame;
    SECTION("wrong size") {
        frame.width = FRAME_WIDTH / 2;
    }
    SECTION("wrong format") {
        frame.format = fm::PixelFormat::RGBA8;
    }
    SECTION("stride smaller than a row") {
        frame.stride = FRAME_WIDTH * PIXEL_SIZE - 1;
    }
    SECTION("stride too large for libavcodec") {
        frame.stride = static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1;
    }
    SECTION("no pixels") {
        frame.pixels = nullptr;
    }
    CHECK(encoder->encode(frame, output) == fm::EncodeStatus::ENCODE_FAILED);
}

TEST_CASE("a hardware encoder starts with an IDR frame carrying SPS and PPS, then sends one P frame per input frame") {
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(), kind);

    for (int i = 0; i < MOVING_FRAME_COUNT; ++i) {
        CAPTURE(i);
        const auto image =
                makeImage(fm::PixelFormat::BGRA8, FRAME_WIDTH, FRAME_HEIGHT, static_cast<std::uint32_t>(i) * BOX_STEP);
        bool keyframe = false;
        const auto accessUnit = encodeCopy(*encoder, image.frame, keyframe);
        if (i == 0) {
            CHECK(keyframe);
            CHECK(isIDRWithParameterSets(accessUnit));
        } else {
            CHECK_FALSE(keyframe);
            CHECK(isDeltaFrame(accessUnit));
        }
    }
}

TEST_CASE("a hardware encoder makes the next frame an IDR frame with SPS and PPS on request") {
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(), kind);
    const auto image = makeImage(fm::PixelFormat::BGRA8, FRAME_WIDTH, FRAME_HEIGHT);
    bool keyframe = false;
    encodeCopy(*encoder, image.frame, keyframe);
    encodeCopy(*encoder, image.frame, keyframe);
    REQUIRE_FALSE(keyframe);

    encoder->requestKeyframe();
    encoder->requestKeyframe();
    const auto requested = encodeCopy(*encoder, image.frame, keyframe);
    CHECK(keyframe);
    CHECK(isIDRWithParameterSets(requested));
    const auto next = encodeCopy(*encoder, image.frame, keyframe);
    CHECK_FALSE(keyframe);
    CHECK(isDeltaFrame(next));
}

TEST_CASE("a hardware encoder adds no IDR frame of its own when the whole picture changes") {
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(), kind);
    const auto calm = makeImage(fm::PixelFormat::BGRA8, FRAME_WIDTH, FRAME_HEIGHT);
    const auto noise = makeNoiseImage();
    bool keyframe = false;
    encodeCopy(*encoder, calm.frame, keyframe);
    REQUIRE(keyframe);

    for (int i = 0; i < SCENE_CHANGE_COUNT; ++i) {
        CAPTURE(i);
        const auto& image = i % 2 == 0 ? noise : calm;
        const auto accessUnit = encodeCopy(*encoder, image.frame, keyframe);
        CHECK_FALSE(keyframe);
        CHECK(isDeltaFrame(accessUnit));
    }
}

TEST_CASE("a hardware encoder refuses a frame above its size limit and makes the next frame an IDR frame") {
    const auto calm = makeImage(fm::PixelFormat::BGRA8, FRAME_WIDTH, FRAME_HEIGHT);
    const auto noise = makeNoiseImage();
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto reference = makeEncoder(kind);
    configureOrSkip(*reference, makeConfig(), kind);
    bool keyframe = false;
    const std::size_t keyframeSize = encodeCopy(*reference, calm.frame, keyframe).size();
    const std::size_t noiseSize = encodeCopy(*reference, noise.frame, keyframe).size();
    CAPTURE(keyframeSize, noiseSize);
    REQUIRE(keyframeSize * MIN_NOISE_TO_KEYFRAME_RATIO < noiseSize);

    const auto encoder = makeEncoder(kind, std::midpoint(keyframeSize, noiseSize));
    configureOrSkip(*encoder, makeConfig(), kind);
    encodeCopy(*encoder, calm.frame, keyframe);
    REQUIRE(keyframe);
    fm::EncodedFrame output;
    CHECK(encoder->encode(noise.frame, output) == fm::EncodeStatus::ENCODE_FAILED);

    const auto next = encodeCopy(*encoder, calm.frame, keyframe);
    CHECK(keyframe);
    CHECK(isIDRWithParameterSets(next));
}

TEST_CASE("an unchanged picture shrinks to a few bytes per frame without filler data") {
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(), kind);
    const auto image = makeImage(fm::PixelFormat::BGRA8, FRAME_WIDTH, FRAME_HEIGHT);
    bool keyframe = false;
    const auto keyframeUnit = encodeCopy(*encoder, image.frame, keyframe);
    CHECK_FALSE(containsNALUnit(nalUnitTypes(keyframeUnit), H264_FILLER_DATA));
    std::vector<std::size_t> frameSizes;
    for (int i = 0; i < STATIC_FRAME_COUNT; ++i) {
        const auto deltaUnit = encodeCopy(*encoder, image.frame, keyframe);
        CHECK_FALSE(containsNALUnit(nalUnitTypes(deltaUnit), H264_FILLER_DATA));
        frameSizes.push_back(deltaUnit.size());
    }
    const auto config = makeConfig();
    const std::size_t frameBudget =
            static_cast<std::size_t>(config.bitrateKbps) * BITS_PER_KILOBIT / BITS_PER_BYTE / config.framesPerSecond;
    CAPTURE(keyframeUnit.size(), frameSizes, frameBudget);
    CHECK(frameSizes.back() * STATIC_BUDGET_FRACTION < frameBudget);
}

TEST_CASE("decoding a hardware encoder's stream gives back the colours of both pixel formats") {
    const auto format = GENERATE(fm::PixelFormat::BGRA8, fm::PixelFormat::RGBA8);
    CAPTURE(format == fm::PixelFormat::BGRA8 ? "BGRA8" : "RGBA8");
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(format), kind);
    TestDecoder decoder;
    openOrSkip(decoder);

    const auto image = makeImage(format, FRAME_WIDTH, FRAME_HEIGHT);
    bool keyframe = false;
    const auto decoded = decoder.decode(encodeCopy(*encoder, image.frame, keyframe));
    REQUIRE(decoded);
    REQUIRE(decoded->width == static_cast<int>(FRAME_WIDTH));
    REQUIRE(decoded->height == static_cast<int>(FRAME_HEIGHT));
    const int left = static_cast<int>(FRAME_WIDTH / 4);
    const int right = static_cast<int>(FRAME_WIDTH * 3 / 4);
    const int top = static_cast<int>(FRAME_HEIGHT / 4);
    const int bottom = static_cast<int>(FRAME_HEIGHT * 3 / 4);
    checkColour(*decoded, left, top, RED);
    checkColour(*decoded, right, top, GREEN);
    checkColour(*decoded, left, bottom, BLUE);
    checkColour(*decoded, right, bottom, WHITE);
}

TEST_CASE("every decoded frame of a hardware encoder shows the picture it was given") {
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(), kind);
    const auto boxCentreY = FRAME_HEIGHT / 2;

    for (int i = 0; i < JUMPING_FRAME_COUNT; ++i) {
        CAPTURE(i);
        const std::uint32_t boxX = static_cast<std::uint32_t>(i) * JUMPING_BOX_STEP;
        const auto image = makeImage(fm::PixelFormat::BGRA8, FRAME_WIDTH, FRAME_HEIGHT, boxX);
        encoder->requestKeyframe();
        bool keyframe = false;
        const auto accessUnit = encodeCopy(*encoder, image.frame, keyframe);
        REQUIRE(keyframe);
        TestDecoder decoder;
        openOrSkip(decoder);
        const auto decoded = decoder.decode(accessUnit);
        REQUIRE(decoded);

        const std::uint32_t boxCentreX = boxX + BOX_SIZE / 2;
        checkColour(*decoded, static_cast<int>(boxCentreX), static_cast<int>(boxCentreY), BLACK);
        if (i > 0) {
            const std::uint32_t previousCentreX = boxCentreX - JUMPING_BOX_STEP;
            checkColour(*decoded, static_cast<int>(previousCentreX), static_cast<int>(boxCentreY),
                        quadrantColour(previousCentreX, boxCentreY, FRAME_WIDTH, FRAME_HEIGHT));
        }
    }
}

TEST_CASE("a hardware encoder crops odd sizes to even ones") {
    constexpr std::uint32_t oddWidth = FRAME_WIDTH + 1;
    constexpr std::uint32_t oddHeight = FRAME_HEIGHT + 1;
    const auto kind = GENERATE(HardwareEncoder::NVENC, HardwareEncoder::VAAPI);
    CAPTURE(nameOf(kind));
    const auto encoder = makeEncoder(kind);
    configureOrSkip(*encoder, makeConfig(fm::PixelFormat::BGRA8, oddWidth, oddHeight), kind);
    TestDecoder decoder;
    openOrSkip(decoder);

    const auto image = makeImage(fm::PixelFormat::BGRA8, oddWidth, oddHeight);
    bool keyframe = false;
    const auto decoded = decoder.decode(encodeCopy(*encoder, image.frame, keyframe));
    REQUIRE(decoded);
    CHECK(decoded->width == static_cast<int>(FRAME_WIDTH));
    CHECK(decoded->height == static_cast<int>(FRAME_HEIGHT));
}
