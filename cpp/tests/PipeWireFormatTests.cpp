#include "linux/PipeWireFormat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {
    const spa_pod* choiceValues(const spa_pod* object, std::uint32_t key, std::uint32_t& valueCount,
                                std::uint32_t& choiceType) {
        const spa_pod_prop* property = ::spa_pod_find_prop(object, nullptr, key);
        if (property == nullptr) {
            return nullptr;
        }
        return ::spa_pod_get_values(&property->value, &valueCount, &choiceType);
    }
}

TEST_CASE("BGR formats with or without alpha map to BGRA8") {
    STATIC_REQUIRE(fm::toPixelFormat(SPA_VIDEO_FORMAT_BGRx) == fm::PixelFormat::BGRA8);
    STATIC_REQUIRE(fm::toPixelFormat(SPA_VIDEO_FORMAT_BGRA) == fm::PixelFormat::BGRA8);
}

TEST_CASE("RGB formats with or without alpha map to RGBA8") {
    STATIC_REQUIRE(fm::toPixelFormat(SPA_VIDEO_FORMAT_RGBx) == fm::PixelFormat::RGBA8);
    STATIC_REQUIRE(fm::toPixelFormat(SPA_VIDEO_FORMAT_RGBA) == fm::PixelFormat::RGBA8);
}

TEST_CASE("formats we did not ask for are rejected") {
    STATIC_REQUIRE_FALSE(fm::toPixelFormat(SPA_VIDEO_FORMAT_xRGB).has_value());
    STATIC_REQUIRE_FALSE(fm::toPixelFormat(SPA_VIDEO_FORMAT_NV12).has_value());
    STATIC_REQUIRE_FALSE(fm::toPixelFormat(SPA_VIDEO_FORMAT_UNKNOWN).has_value());
}

TEST_CASE("the capture size limit still covers 8K screens") {
    STATIC_REQUIRE(fm::MAX_CAPTURE_SIZE >= 7680);
}

TEST_CASE("the format offer fits the pod buffer and offers what we expect") {
    std::uint8_t podBuffer[fm::POD_BUFFER_SIZE];
    spa_pod_builder builder{};
    ::spa_pod_builder_init(&builder, podBuffer, sizeof(podBuffer));

    const spa_pod* offer = fm::buildFormatParameter(builder, 60);
    REQUIRE(offer != nullptr);

    std::uint32_t mediaType = 0;
    std::uint32_t mediaSubtype = 0;
    REQUIRE(::spa_format_parse(offer, &mediaType, &mediaSubtype) >= 0);
    CHECK(mediaType == SPA_MEDIA_TYPE_video);
    CHECK(mediaSubtype == SPA_MEDIA_SUBTYPE_raw);

    SECTION("five pixel formats, BGRx preferred") {
        std::uint32_t valueCount = 0;
        std::uint32_t choiceType = 0;
        const spa_pod* values = choiceValues(offer, SPA_FORMAT_VIDEO_format, valueCount, choiceType);
        REQUIRE(values != nullptr);
        CHECK(choiceType == SPA_CHOICE_Enum);
        CHECK(valueCount == 5);
        std::uint32_t preferred = 0;
        REQUIRE(::spa_pod_get_id(values, &preferred) >= 0);
        CHECK(preferred == SPA_VIDEO_FORMAT_BGRx);
    }
    SECTION("sizes up to MAX_CAPTURE_SIZE") {
        std::uint32_t valueCount = 0;
        std::uint32_t choiceType = 0;
        const spa_pod* values = choiceValues(offer, SPA_FORMAT_VIDEO_size, valueCount, choiceType);
        REQUIRE(values != nullptr);
        CHECK(choiceType == SPA_CHOICE_Range);
        REQUIRE(valueCount == 3);
        const auto* sizes = static_cast<const spa_rectangle*>(SPA_POD_BODY_CONST(values));
        CHECK(sizes[2].width == fm::MAX_CAPTURE_SIZE);
        CHECK(sizes[2].height == fm::MAX_CAPTURE_SIZE);
    }
    SECTION("at most the requested frame rate") {
        std::uint32_t valueCount = 0;
        std::uint32_t choiceType = 0;
        const spa_pod* values = choiceValues(offer, SPA_FORMAT_VIDEO_maxFramerate, valueCount, choiceType);
        REQUIRE(values != nullptr);
        CHECK(choiceType == SPA_CHOICE_Range);
        REQUIRE(valueCount == 3);
        const auto* rates = static_cast<const spa_fraction*>(SPA_POD_BODY_CONST(values));
        CHECK(rates[2].num == 60);
        CHECK(rates[2].denom == 1);
    }
}

TEST_CASE("the buffer and timestamp parameters fit the pod buffer together") {
    std::uint8_t podBuffer[fm::POD_BUFFER_SIZE];
    spa_pod_builder builder{};
    ::spa_pod_builder_init(&builder, podBuffer, sizeof(podBuffer));

    CHECK(fm::buildBufferParameter(builder) != nullptr);
    CHECK(fm::buildTimestampParameter(builder) != nullptr);
}

TEST_CASE("a pod buffer that is too small gives nullptr instead of a broken parameter") {
    std::uint8_t podBuffer[16];
    spa_pod_builder builder{};
    ::spa_pod_builder_init(&builder, podBuffer, sizeof(podBuffer));

    SECTION("format offer") {
        CHECK(fm::buildFormatParameter(builder, 60) == nullptr);
    }
    SECTION("buffer parameter") {
        CHECK(fm::buildBufferParameter(builder) == nullptr);
    }
    SECTION("timestamp parameter") {
        CHECK(fm::buildTimestampParameter(builder) == nullptr);
    }
}
