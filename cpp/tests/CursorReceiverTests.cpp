#include "CursorPackets.hpp"
#include "CursorReceiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
    using Clock = std::chrono::steady_clock;

    const Clock::time_point START = Clock::time_point{} + 100s;
    constexpr std::uint32_t SESSION_ID = 77;

    fm::CursorImage makeImage(std::uint16_t width, std::uint16_t height, std::uint8_t seed) {
        fm::CursorImage image{.width = width,
                              .height = height,
                              .hotspotX = std::min<std::uint16_t>(1, width - 1),
                              .hotspotY = std::min<std::uint16_t>(2, height - 1),
                              .pixels = std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
        for (std::size_t i = 0; i < image.pixels.size(); ++i) {
            image.pixels[i] = static_cast<std::uint8_t>(i * 7 + seed);
        }
        return image;
    }

    std::vector<std::vector<std::uint8_t>> shapePackets(const fm::CursorImage& image, std::uint32_t shapeId,
                                                        std::uint32_t firstSeq) {
        std::vector<std::vector<std::uint8_t>> packets;
        const std::uint16_t count = fm::cursorShapeFragmentCount(image);
        REQUIRE(count > 0);
        for (std::uint16_t index = 0; index < count; ++index) {
            std::vector<std::uint8_t> bytes(fm::MAX_PACKET_SIZE);
            const std::size_t size =
                    fm::writeCursorShapeFragment(image, shapeId, index, SESSION_ID, firstSeq + index, 0, bytes);
            REQUIRE(size != 0);
            bytes.resize(size);
            packets.push_back(std::move(bytes));
        }
        return packets;
    }

    fm::CursorShape parse(const std::vector<std::uint8_t>& bytes) {
        fm::CursorShape fragment;
        REQUIRE(fragment.read(bytes));
        return fragment;
    }

    void receiveShape(fm::CursorReceiver& receiver, const fm::CursorImage& image, std::uint32_t shapeId,
                      std::uint32_t firstSeq, Clock::time_point now = START) {
        for (const auto& bytes : shapePackets(image, shapeId, firstSeq)) {
            REQUIRE(receiver.acceptShapeFragment(parse(bytes), now));
        }
    }

    fm::CursorPos position(std::uint32_t seq, std::uint32_t shapeId, std::int32_t x, std::int32_t y,
                           bool visible = true) {
        fm::CursorPos packet;
        packet.commonHeader.sessionId = SESSION_ID;
        packet.commonHeader.seq = seq;
        packet.shapeId = shapeId;
        packet.x = x;
        packet.y = y;
        packet.flags = visible ? fm::CursorFlags::VISIBLE : std::uint8_t{0};
        return packet;
    }

    fm::CursorSnapshot snapshotOf(const fm::CursorReceiver& receiver) {
        return fm::CursorSnapshot::unpack(receiver.snapshot());
    }

    std::vector<std::uint32_t> expectedARGB(const fm::CursorImage& image) {
        std::vector<std::uint32_t> colors;
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            colors.push_back((std::uint32_t{image.pixels[i + 3]} << 24) | (std::uint32_t{image.pixels[i + 2]} << 16) |
                             (std::uint32_t{image.pixels[i + 1]} << 8) | std::uint32_t{image.pixels[i]});
        }
        return colors;
    }

    std::optional<std::vector<std::uint32_t>> drawnPixels(const fm::CursorReceiver& receiver) {
        std::vector<std::uint32_t> pixels(fm::CursorShapeCache::MAX_PIXELS);
        const auto info = receiver.copyShape(receiver.snapshot(), pixels);
        if (!info) {
            return std::nullopt;
        }
        pixels.resize(std::size_t{info->width} * info->height);
        return pixels;
    }
}

TEST_CASE("a shape and then a position publish the cursor, with the pixels as Android ARGB colors") {
    fm::CursorReceiver receiver;
    CHECK(snapshotOf(receiver) == fm::CursorSnapshot{});

    const auto image = makeImage(24, 24, 3);
    receiveShape(receiver, image, 7, 100);
    CHECK_FALSE(snapshotOf(receiver).visible);

    receiver.acceptPosition(position(102, 7, 300, -4));
    const auto snapshot = snapshotOf(receiver);
    CHECK(snapshot.visible);
    CHECK(snapshot.x == 300);
    CHECK(snapshot.y == -4);
    CHECK(snapshot.slot != fm::CursorSnapshot::NO_SLOT);

    std::vector<std::uint32_t> pixels(fm::CursorShapeCache::MAX_PIXELS);
    const auto info = receiver.copyShape(receiver.snapshot(), pixels);
    REQUIRE(info);
    CHECK(*info == fm::CursorShapeInfo{.width = 24, .height = 24, .hotspotX = 1, .hotspotY = 2});
    CHECK(drawnPixels(receiver) == expectedARGB(image));

    SECTION("the bytes blue, green, red, alpha become the color 0xAARRGGBB") {
        const fm::CursorImage onePixel{
                .width = 1, .height = 1, .hotspotX = 0, .hotspotY = 0, .pixels = {0x10, 0x20, 0x30, 0x40}};
        receiveShape(receiver, onePixel, 8, 200);
        receiver.acceptPosition(position(202, 8, 0, 0));
        CHECK(drawnPixels(receiver) == std::vector<std::uint32_t>{0x40302010});
    }
}

TEST_CASE("the snapshot packs every field into one 64-bit value and back") {
    const fm::CursorSnapshot snapshot{.x = -20, .y = 32767, .visible = true, .slot = 15, .generation = 0xFFFE};
    CHECK(fm::CursorSnapshot::unpack(snapshot.pack()) == snapshot);
    CHECK(fm::CursorSnapshot::unpack(fm::CursorSnapshot{}.pack()) == fm::CursorSnapshot{});
}

TEST_CASE("the packed snapshot has the bit layout that the Android app's CursorState reads") {
    const fm::CursorSnapshot snapshot{.x = -2, .y = 3, .visible = true, .slot = 5, .generation = 0x1234};
    CHECK(snapshot.pack() == 0x1234'0105'0003'FFFEULL);
    CHECK(fm::CursorSnapshot{}.pack() == 0x0000'00FF'0000'0000ULL);
}

TEST_CASE("coordinates beyond 16 bits are clamped, so a far-away cursor stays far away") {
    fm::CursorReceiver receiver;
    receiveShape(receiver, makeImage(4, 4, 1), 1, 10);
    receiver.acceptPosition(position(20, 1, 100000, -100000, false));
    CHECK(snapshotOf(receiver).x == 32767);
    CHECK(snapshotOf(receiver).y == -32768);
}

TEST_CASE("positions are ordered by seq across the wrap-around; older and repeated ones are dropped") {
    fm::CursorReceiver receiver;
    receiveShape(receiver, makeImage(4, 4, 1), 1, 10);
    receiver.acceptPosition(position(0xFFFFFFFE, 1, 10, 10));
    receiver.acceptPosition(position(0xFFFFFFFF, 1, 11, 10));
    CHECK(snapshotOf(receiver).x == 11);
    receiver.acceptPosition(position(0, 1, 12, 10));
    CHECK(snapshotOf(receiver).x == 12);
    receiver.acceptPosition(position(0xFFFFFFFF, 1, 99, 99));
    receiver.acceptPosition(position(0, 1, 98, 98));
    CHECK(snapshotOf(receiver).x == 12);
    CHECK(snapshotOf(receiver).y == 10);
}

TEST_CASE("a position for a missing shape keeps the last shape and asks for the missing one every 500 ms") {
    fm::CursorReceiver receiver;
    const auto arrow = makeImage(24, 24, 1);
    const auto beam = makeImage(8, 16, 9);
    receiveShape(receiver, arrow, 1, 10);
    receiver.acceptPosition(position(20, 1, 5, 5));
    const std::uint8_t arrowSlot = snapshotOf(receiver).slot;
    CHECK_FALSE(receiver.takeShapeRequest(START));

    receiver.acceptPosition(position(21, 2, 6, 6));
    CHECK(snapshotOf(receiver).slot == arrowSlot);
    CHECK(snapshotOf(receiver).visible);
    CHECK(snapshotOf(receiver).x == 6);
    CHECK(receiver.takeShapeRequest(START) == 2U);

    receiver.acceptPosition(position(22, 2, 7, 7));
    CHECK_FALSE(receiver.takeShapeRequest(START + 499ms));
    CHECK(receiver.takeShapeRequest(START + 500ms) == 2U);

    receiveShape(receiver, beam, 2, 30, START + 600ms);
    CHECK(snapshotOf(receiver).slot != arrowSlot);
    CHECK(snapshotOf(receiver).x == 7);
    CHECK(drawnPixels(receiver) == expectedARGB(beam));
    CHECK_FALSE(receiver.takeShapeRequest(START + 2s));
}

TEST_CASE("without any shape the cursor is not drawn, and its shape is requested") {
    fm::CursorReceiver receiver;
    receiver.acceptPosition(position(1, 4, 50, 60));
    const auto snapshot = snapshotOf(receiver);
    CHECK_FALSE(snapshot.visible);
    CHECK(snapshot.slot == fm::CursorSnapshot::NO_SLOT);
    CHECK(snapshot.x == 50);
    CHECK(receiver.takeShapeRequest(START) == 4U);
    CHECK_FALSE(drawnPixels(receiver));
}

TEST_CASE("fragments in any order, some of them twice, complete the shape once all have arrived") {
    fm::CursorReceiver receiver;
    const auto image = makeImage(40, 40, 5);
    const auto packets = shapePackets(image, 3, 50);
    REQUIRE(packets.size() == 5);
    receiver.acceptPosition(position(40, 3, 1, 1));
    for (const std::size_t index : {4U, 2U, 2U, 0U, 3U}) {
        CHECK(receiver.acceptShapeFragment(parse(packets[index]), START));
    }
    CHECK_FALSE(snapshotOf(receiver).visible);
    CHECK(receiver.acceptShapeFragment(parse(packets[1]), START));
    CHECK(snapshotOf(receiver).visible);
    CHECK(drawnPixels(receiver) == expectedARGB(image));
}

TEST_CASE("fragments that break the shape rules are rejected") {
    const auto small = shapePackets(makeImage(2, 2, 1), 1, 10);
    const auto large = shapePackets(makeImage(24, 24, 1), 2, 20);
    const std::vector<std::pair<const char*, std::function<fm::CursorShape()>>> cases{
            {"width 0",
             [&] {
                 auto f = parse(small[0]);
                 f.width = 0;
                 return f;
             }},
            {"width 257",
             [&] {
                 auto f = parse(small[0]);
                 f.width = 257;
                 return f;
             }},
            {"height 0",
             [&] {
                 auto f = parse(small[0]);
                 f.height = 0;
                 return f;
             }},
            {"height 257",
             [&] {
                 auto f = parse(small[0]);
                 f.height = 257;
                 return f;
             }},
            {"hotspot x outside",
             [&] {
                 auto f = parse(small[0]);
                 f.hotspotX = 2;
                 return f;
             }},
            {"hotspot y outside",
             [&] {
                 auto f = parse(small[0]);
                 f.hotspotY = 2;
                 return f;
             }},
            {"fragCount too high",
             [&] {
                 auto f = parse(small[0]);
                 f.fragCount = 2;
                 return f;
             }},
            {"fragIndex beyond fragCount",
             [&] {
                 auto f = parse(small[0]);
                 f.fragIndex = 1;
                 return f;
             }},
            {"payload one byte short",
             [&] {
                 auto f = parse(small[0]);
                 f.payload = f.payload.first(15);
                 return f;
             }},
            {"first of two fragments short",
             [&] {
                 auto f = parse(large[0]);
                 f.payload = f.payload.first(fm::MAX_SHAPE_PAYLOAD - 1);
                 return f;
             }},
            {"last fragment short",
             [&] {
                 auto f = parse(large[1]);
                 f.payload = f.payload.first(f.payload.size() - 1);
                 return f;
             }},
    };
    for (const auto& [name, makeFragment] : cases) {
        CAPTURE(name);
        fm::CursorReceiver receiver;
        CHECK_FALSE(receiver.acceptShapeFragment(makeFragment(), START));
    }
}

TEST_CASE("a fragment that disagrees with earlier fragments of the same shape is rejected") {
    fm::CursorReceiver receiver;
    auto image = makeImage(24, 24, 1);
    const auto packets = shapePackets(image, 5, 10);
    image.hotspotX = 9;
    const auto otherHotspot = shapePackets(image, 5, 12);
    const auto otherSize = shapePackets(makeImage(40, 40, 1), 5, 14);
    receiver.acceptPosition(position(9, 5, 0, 0));

    CHECK(receiver.acceptShapeFragment(parse(packets[0]), START));
    CHECK_FALSE(receiver.acceptShapeFragment(parse(otherHotspot[1]), START));
    CHECK_FALSE(receiver.acceptShapeFragment(parse(otherSize[1]), START));
    CHECK_FALSE(snapshotOf(receiver).visible);
    CHECK(receiver.acceptShapeFragment(parse(packets[1]), START));
    CHECK(snapshotOf(receiver).visible);
}

TEST_CASE("an incomplete shape is dropped when none of its fragments arrived for FRAGMENT_TIMEOUT_MS") {
    fm::CursorReceiver receiver;
    const auto packets = shapePackets(makeImage(24, 24, 1), 6, 10);
    receiver.acceptPosition(position(9, 6, 0, 0));

    SECTION("exactly the timeout still completes it") {
        REQUIRE(receiver.acceptShapeFragment(parse(packets[0]), START));
        REQUIRE(receiver.acceptShapeFragment(parse(packets[1]), START + 100ms));
        CHECK(snapshotOf(receiver).visible);
    }
    SECTION("a fragment after the timeout starts over") {
        REQUIRE(receiver.acceptShapeFragment(parse(packets[0]), START));
        REQUIRE(receiver.acceptShapeFragment(parse(packets[1]), START + 101ms));
        CHECK_FALSE(snapshotOf(receiver).visible);
        REQUIRE(receiver.acceptShapeFragment(parse(packets[0]), START + 150ms));
        CHECK(snapshotOf(receiver).visible);
    }
}

TEST_CASE("a late fragment of an older shape does not replace the newer shape being assembled") {
    fm::CursorReceiver receiver;
    const auto older = shapePackets(makeImage(24, 24, 1), 1, 10);
    const auto newer = shapePackets(makeImage(24, 24, 2), 2, 20);
    receiver.acceptPosition(position(30, 2, 0, 0));

    CHECK(receiver.acceptShapeFragment(parse(newer[0]), START));
    CHECK(receiver.acceptShapeFragment(parse(older[1]), START));
    CHECK(receiver.acceptShapeFragment(parse(newer[1]), START));
    CHECK(snapshotOf(receiver).visible);
}

TEST_CASE("the cache keeps 16 shapes and replaces the least recently used one, never the one being drawn") {
    fm::CursorReceiver receiver;
    std::uint32_t seq = 1000;
    receiveShape(receiver, makeImage(2, 2, 1), 1, seq);
    seq += 10;
    receiver.acceptPosition(position(seq++, 1, 3, 3));
    const auto drawn = snapshotOf(receiver);
    for (std::uint32_t shapeId = 2; shapeId <= fm::MIN_SHAPE_CACHE_SIZE + 1; ++shapeId) {
        receiveShape(receiver, makeImage(2, 2, static_cast<std::uint8_t>(shapeId)), shapeId, seq);
        seq += 10;
    }
    CHECK(snapshotOf(receiver) == drawn);
    CHECK(drawnPixels(receiver) == expectedARGB(makeImage(2, 2, 1)));

    receiver.acceptPosition(position(seq++, 2, 3, 3));
    CHECK(receiver.takeShapeRequest(START) == 2U);
    receiver.acceptPosition(position(seq++, 3, 3, 3));
    CHECK(drawnPixels(receiver) == expectedARGB(makeImage(2, 2, 3)));
    receiver.acceptPosition(position(seq++, 1, 3, 3));
    CHECK(drawnPixels(receiver) == expectedARGB(makeImage(2, 2, 1)));
}

TEST_CASE("fragments of a shape that is already cached change nothing") {
    fm::CursorReceiver receiver;
    const auto image = makeImage(24, 24, 1);
    receiveShape(receiver, image, 1, 10);
    receiver.acceptPosition(position(20, 1, 3, 3));
    const auto before = receiver.snapshot();
    receiveShape(receiver, image, 1, 30);
    CHECK(receiver.snapshot() == before);
}

TEST_CASE("reset forgets the shapes and the last seq and hides the cursor") {
    fm::CursorReceiver receiver;
    receiveShape(receiver, makeImage(4, 4, 1), 1, 10);
    receiver.acceptPosition(position(500, 1, 3, 3));
    receiver.reset();
    CHECK(snapshotOf(receiver) == fm::CursorSnapshot{});

    receiver.acceptPosition(position(5, 1, 4, 4));
    CHECK(snapshotOf(receiver).x == 4);
    CHECK_FALSE(snapshotOf(receiver).visible);
    CHECK(receiver.takeShapeRequest(START) == 1U);
}
