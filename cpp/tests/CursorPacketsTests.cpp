#include "CursorPackets.hpp"
#include "Packets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {
    constexpr std::uint32_t SESSION_ID = 0x1122'3344;

    fm::CursorImage makeImage(std::uint16_t width, std::uint16_t height) {
        fm::CursorImage image{.width = width,
                              .height = height,
                              .hotspotX = 3,
                              .hotspotY = 4,
                              .pixels = std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
        for (std::size_t index = 0; index < image.pixels.size(); ++index) {
            image.pixels[index] = static_cast<std::uint8_t>(index * 7 + index / 253);
        }
        return image;
    }

    std::vector<std::uint8_t> bytesAt(std::span<const std::uint8_t> packet, std::size_t offset, std::size_t count) {
        return {packet.begin() + static_cast<std::ptrdiff_t>(offset),
                packet.begin() + static_cast<std::ptrdiff_t>(offset + count)};
    }
}

TEST_CASE("CURSOR_POS carries the position, the image as its shape and the VISIBLE flag at the spec's offsets") {
    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    const fm::CursorState state{.x = -5, .y = 1840, .visible = true, .imageId = 3};
    const std::size_t size = fm::writeCursorPos(state, SESSION_ID, 99, 123456, buffer);
    REQUIRE(size == fm::CursorPos::SIZE);
    const auto packet = std::span<const std::uint8_t>(buffer).first(size);

    CHECK(packet[3] == static_cast<std::uint8_t>(fm::PacketType::CURSOR_POS));
    CHECK(bytesAt(packet, 24, 4) == std::vector<std::uint8_t>{0, 0, 0, 3});
    CHECK(bytesAt(packet, 28, 4) == std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFB});
    CHECK(bytesAt(packet, 32, 4) == std::vector<std::uint8_t>{0, 0, 0x07, 0x30});
    CHECK(bytesAt(packet, 36, 4) == std::vector<std::uint8_t>{0x01, 0, 0, 0});

    fm::CursorPos parsed;
    REQUIRE(parsed.read(packet));
    CHECK(parsed.commonHeader.magic == fm::MAGIC);
    CHECK(parsed.commonHeader.version == fm::PROTOCOL_VERSION);
    CHECK(parsed.commonHeader.sessionId == SESSION_ID);
    CHECK(parsed.commonHeader.seq == 99);
    CHECK(parsed.commonHeader.timestampUs == 123456);
    CHECK(parsed.shapeId == 3);
    CHECK(parsed.x == -5);
    CHECK(parsed.y == 1840);
}

TEST_CASE("a hidden cursor clears VISIBLE, and a buffer that is too small writes nothing") {
    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    const fm::CursorState hidden{.x = 1, .y = 2, .visible = false, .imageId = 1};
    REQUIRE(fm::writeCursorPos(hidden, SESSION_ID, 1, 1, buffer) == fm::CursorPos::SIZE);
    CHECK(buffer[36] == 0);

    std::array<std::uint8_t, fm::CursorPos::SIZE - 1> small{};
    CHECK(fm::writeCursorPos(hidden, SESSION_ID, 1, 1, small) == 0);
}

TEST_CASE("a 32x32 cursor is split as in the spec's example: 1360, 1360, 1360 and 16 bytes") {
    const fm::CursorImage image = makeImage(32, 32);
    REQUIRE(fm::cursorShapeFragmentCount(image) == 4);
    constexpr std::array<std::size_t, 4> payloadSizes{1360, 1360, 1360, 16};

    std::vector<std::uint8_t> joined;
    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    for (std::uint16_t fragIndex = 0; fragIndex < 4; ++fragIndex) {
        const std::size_t size =
                fm::writeCursorShapeFragment(image, 9, fragIndex, SESSION_ID, 40U + fragIndex, 777, buffer);
        REQUIRE(size == fm::CursorShape::SIZE + payloadSizes[fragIndex]);
        fm::CursorShape parsed;
        REQUIRE(parsed.read(std::span<const std::uint8_t>(buffer).first(size)));
        CHECK(parsed.commonHeader.type == static_cast<std::uint8_t>(fm::PacketType::CURSOR_SHAPE));
        CHECK(parsed.commonHeader.sessionId == SESSION_ID);
        CHECK(parsed.commonHeader.seq == 40U + fragIndex);
        CHECK(parsed.shapeId == 9);
        CHECK(parsed.fragIndex == fragIndex);
        CHECK(parsed.fragCount == 4);
        CHECK(parsed.width == 32);
        CHECK(parsed.height == 32);
        CHECK(parsed.hotspotX == 3);
        CHECK(parsed.hotspotY == 4);
        joined.insert(joined.end(), parsed.payload.begin(), parsed.payload.end());
    }
    CHECK(joined == image.pixels);
}

TEST_CASE("a 24x24 arrow takes two fragments") {
    const fm::CursorImage image = makeImage(24, 24);
    CHECK(fm::cursorShapeFragmentCount(image) == 2);
    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    CHECK(fm::writeCursorShapeFragment(image, 1, 1, SESSION_ID, 0, 0, buffer) == fm::CursorShape::SIZE + 944);
    CHECK(fm::writeCursorShapeFragment(image, 1, 2, SESSION_ID, 0, 0, buffer) == 0);
}

TEST_CASE("an image that breaks the spec's shape rules is never written") {
    fm::CursorImage image = makeImage(24, 24);
    SECTION("no width") {
        image.width = 0;
    }
    SECTION("wider than 256") {
        image = makeImage(257, 1);
        image.hotspotX = 0;
        image.hotspotY = 0;
    }
    SECTION("hotspot outside the image") {
        image.hotspotY = 24;
    }
    SECTION("pixel data of the wrong size") {
        image.pixels.pop_back();
    }
    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    CHECK(fm::cursorShapeFragmentCount(image) == 0);
    CHECK(fm::writeCursorShapeFragment(image, 1, 0, SESSION_ID, 0, 0, buffer) == 0);
}

TEST_CASE("a shape fragment needs room for its header and payload") {
    const fm::CursorImage image = makeImage(24, 24);
    std::array<std::uint8_t, fm::CursorShape::SIZE + fm::MAX_SHAPE_PAYLOAD - 1> small{};
    CHECK(fm::writeCursorShapeFragment(image, 1, 0, SESSION_ID, 0, 0, small) == 0);
}
