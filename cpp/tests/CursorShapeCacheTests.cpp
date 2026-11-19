#include "CursorReceiver.hpp"
#include "CursorShapeCache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace {
    constexpr fm::CursorShapeInfo TWO_PIXELS{.width = 2, .height = 1, .hotspotX = 1, .hotspotY = 0};
    const std::vector<std::uint8_t> TWO_PIXELS_BGRA{1, 2, 3, 4, 5, 6, 7, 8};
}

TEST_CASE("a stored shape is copied for its generation only, and only into a buffer that fits") {
    fm::CursorShapeCache cache;
    const auto slot = cache.store(9, TWO_PIXELS, TWO_PIXELS_BGRA, std::nullopt);
    REQUIRE(slot);
    CHECK(cache.find(9) == slot);
    CHECK_FALSE(cache.find(10));
    const std::uint16_t generation = cache.generation(*slot);
    CHECK(generation != 0);
    CHECK(generation % 2 == 0);

    std::vector<std::uint32_t> pixels(2);
    CHECK(cache.copy(*slot, generation, pixels) == TWO_PIXELS);
    CHECK(pixels == std::vector<std::uint32_t>{0x04030201, 0x08070605});

    CHECK_FALSE(cache.copy(*slot, static_cast<std::uint16_t>(generation + 2), pixels));
    std::vector<std::uint32_t> tooSmall(1);
    CHECK_FALSE(cache.copy(*slot, generation, tooSmall));
    CHECK_FALSE(cache.copy(fm::CursorSnapshot::NO_SLOT, generation, pixels));
}

TEST_CASE("store refuses pixels that do not match the size") {
    fm::CursorShapeCache cache;
    const std::vector<std::uint8_t> tooFewBytes(7);
    CHECK_FALSE(cache.store(1, TWO_PIXELS, tooFewBytes, std::nullopt));
    CHECK_FALSE(cache.store(1, fm::CursorShapeInfo{}, {}, std::nullopt));
    CHECK_FALSE(cache.find(1));
}

TEST_CASE("clear forgets every shape") {
    fm::CursorShapeCache cache;
    REQUIRE(cache.store(1, TWO_PIXELS, TWO_PIXELS_BGRA, std::nullopt));
    cache.clear();
    CHECK_FALSE(cache.find(1));
}

TEST_CASE("a reader on another thread never gets a half-written shape") {
    constexpr std::uint16_t SIDE = 64;
    constexpr std::uint32_t ROUNDS = 2000;
    fm::CursorShapeCache cache;
    std::atomic<std::uint64_t> latest{fm::CursorSnapshot{}.pack()};
    std::atomic<bool> done{false};

    std::thread writer([&] {
        std::vector<std::uint8_t> bgra(std::size_t{SIDE} * SIDE * fm::CursorShapeCache::BYTES_PER_PIXEL);
        std::optional<std::uint8_t> drawn;
        for (std::uint32_t round = 1; round <= ROUNDS; ++round) {
            std::ranges::fill(bgra, static_cast<std::uint8_t>(round));
            drawn = cache.store(round, fm::CursorShapeInfo{.width = SIDE, .height = SIDE, .hotspotX = 0, .hotspotY = 0},
                                bgra, drawn);
            if (drawn) {
                latest.store(
                        fm::CursorSnapshot{
                                .x = 0, .y = 0, .visible = true, .slot = *drawn, .generation = cache.generation(*drawn)}
                                .pack(),
                        std::memory_order_release);
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::vector<std::uint32_t> pixels(std::size_t{SIDE} * SIDE);
    std::uint64_t mixed = 0;
    while (!done.load(std::memory_order_acquire)) {
        const auto snapshot = fm::CursorSnapshot::unpack(latest.load(std::memory_order_acquire));
        if (snapshot.slot == fm::CursorSnapshot::NO_SLOT || !cache.copy(snapshot.slot, snapshot.generation, pixels)) {
            continue;
        }
        if (!std::ranges::all_of(pixels, [&](std::uint32_t pixel) { return pixel == pixels[0]; })) {
            ++mixed;
        }
    }
    writer.join();
    const auto last = fm::CursorSnapshot::unpack(latest.load(std::memory_order_acquire));
    REQUIRE(cache.copy(last.slot, last.generation, pixels));
    CHECK(pixels[0] == 0xD0D0D0D0);
    CHECK(mixed == 0);
}
