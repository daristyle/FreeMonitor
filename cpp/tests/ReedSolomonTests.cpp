#include "ReedSolomon.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace {
    using Bytes = std::vector<std::uint8_t>;

    Bytes makeFrame(std::size_t size, std::uint32_t seed) {
        std::mt19937 random(seed);
        Bytes frame(size);
        std::ranges::generate(frame, [&random] { return static_cast<std::uint8_t>(random()); });
        return frame;
    }

    Bytes parityOf(const Bytes& frame, std::uint16_t parityIndex) {
        Bytes parity(fm::fecBlockSize(static_cast<std::uint32_t>(frame.size())));
        REQUIRE(fm::computeParity(frame, parityIndex, parity));
        return parity;
    }

    void damageFragment(Bytes& frame, std::uint16_t fragIndex) {
        const std::size_t offset = std::size_t{fragIndex} * fm::MAX_FRAGMENT_PAYLOAD;
        const std::size_t size = std::min<std::size_t>(fm::MAX_FRAGMENT_PAYLOAD, frame.size() - offset);
        std::fill_n(frame.begin() + static_cast<std::ptrdiff_t>(offset), size, std::uint8_t{0xEE});
    }

    bool recoverWith(Bytes& damaged, const Bytes& original, const std::vector<std::uint16_t>& missing,
                     const std::vector<std::uint16_t>& parityIndices) {
        std::vector<Bytes> blocks;
        for (const std::uint16_t parityIndex : parityIndices) {
            blocks.push_back(parityOf(original, parityIndex));
        }
        std::vector<fm::ParityBlock> parity;
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            parity.push_back(fm::ParityBlock{parityIndices[index], blocks[index]});
        }
        return fm::recoverFragments(damaged, missing, parity);
    }

    std::vector<std::vector<std::uint16_t>> combinations(std::uint16_t count, std::size_t size) {
        std::vector<std::vector<std::uint16_t>> result;
        std::vector<bool> chosen(count);
        std::fill_n(chosen.begin(), size, true);
        do {
            std::vector<std::uint16_t> combination;
            for (std::uint16_t index = 0; index < count; ++index) {
                if (chosen[index]) {
                    combination.push_back(index);
                }
            }
            result.push_back(combination);
        } while (std::prev_permutation(chosen.begin(), chosen.end()));
        return result;
    }

    std::uint8_t multiplySlowly(std::uint8_t left, std::uint8_t right) {
        std::uint8_t product = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((right >> bit & 1) != 0) {
                product ^= left;
            }
            const bool overflow = (left & 0x80) != 0;
            left = static_cast<std::uint8_t>(left << 1);
            if (overflow) {
                left ^= 0x1D;
            }
        }
        return product;
    }
}

TEST_CASE("field multiplication matches a slow reference for every pair") {
    for (unsigned left = 0; left < 256; ++left) {
        for (unsigned right = 0; right < 256; ++right) {
            const auto a = static_cast<std::uint8_t>(left);
            const auto b = static_cast<std::uint8_t>(right);
            REQUIRE(fm::gfMultiply(a, b) == multiplySlowly(a, b));
        }
    }
}

TEST_CASE("every non-zero field element has an inverse") {
    for (unsigned value = 1; value < 256; ++value) {
        const auto element = static_cast<std::uint8_t>(value);
        REQUIRE(fm::gfMultiply(element, fm::gfInverse(element)) == 1);
    }
}

TEST_CASE("field arithmetic and coefficients match the spec's examples") {
    CHECK(fm::gfMultiply(0x02, 0x80) == 0x1D);
    CHECK(fm::parityCoefficient(0, 0) == 0x6C);
    CHECK(fm::parityCoefficient(0, 1) == 0xED);
    CHECK(fm::parityCoefficient(15, 31) == 0x48);
}

TEST_CASE("parity of a three-byte frame matches the spec's example") {
    const Bytes frame{1, 2, 3};
    CHECK(parityOf(frame, 0) == Bytes{0x6C, 0xD8, 0xB4});
    CHECK(parityOf(frame, 1) == Bytes{0xED, 0xC7, 0x2A});
}

TEST_CASE("the short last fragment counts as padded with zeros") {
    Bytes frame(fm::MAX_FRAGMENT_PAYLOAD + 2, 0x11);
    frame[fm::MAX_FRAGMENT_PAYLOAD] = 0xAA;
    frame[fm::MAX_FRAGMENT_PAYLOAD + 1] = 0xBB;
    const Bytes parity = parityOf(frame, 0);
    REQUIRE(parity.size() == fm::MAX_FRAGMENT_PAYLOAD);
    CHECK(Bytes(parity.begin(), parity.begin() + 4) == Bytes{0x65, 0xFE, 0xE2, 0xE2});
}

TEST_CASE("a frame's fragments are spread over groups of at most MAX_FEC_GROUP_SIZE") {
    CHECK(fm::fecGroupCount(1) == 1);
    CHECK(fm::fecGroupCount(32) == 1);
    CHECK(fm::fecGroupCount(33) == 2);
    CHECK(fm::fecGroupCount(3103) == 97);
    CHECK(fm::fecBlockSize(3) == 3);
    CHECK(fm::fecBlockSize(fm::MAX_FRAGMENT_PAYLOAD + 1) == fm::MAX_FRAGMENT_PAYLOAD);
}

TEST_CASE("the parity count follows the percentage, with a floor per frame and a limit per group") {
    CHECK(fm::fecParityCount(0, 30) == 0);
    CHECK(fm::fecParityCount(10, 0) == 0);
    CHECK(fm::fecParityCount(1, 30) == 2);
    CHECK(fm::fecParityCount(2, 30) == 2);
    CHECK(fm::fecParityCount(7, 30) == 3);
    CHECK(fm::fecParityCount(19, 30) == 6);
    CHECK(fm::fecParityCount(32, 30) == 10);
    CHECK(fm::fecParityCount(178, 30) == 54);
    CHECK(fm::fecParityCount(32, 100) == 16);
    CHECK(fm::fecParityCount(33, 100) == 32);
    CHECK(fm::fecParityCount(3103, 100) == 1552);
}

TEST_CASE("any lost fragments are rebuilt from any parity packets of the same number") {
    constexpr std::uint16_t fragments = 5;
    constexpr std::uint16_t parityPackets = 4;
    const Bytes frame = makeFrame(fragments * fm::MAX_FRAGMENT_PAYLOAD - 100, 1);

    const std::size_t lost = GENERATE(1U, 2U, 3U, 4U);
    for (const auto& missing : combinations(fragments, lost)) {
        for (const auto& parityIndices : combinations(parityPackets, lost)) {
            Bytes damaged = frame;
            for (const std::uint16_t fragIndex : missing) {
                damageFragment(damaged, fragIndex);
            }
            REQUIRE(recoverWith(damaged, frame, missing, parityIndices));
            REQUIRE(damaged == frame);
        }
    }
}

TEST_CASE("a full group rebuilds as many fragments as it has parity rows") {
    constexpr std::uint16_t fragments = fm::MAX_FEC_GROUP_SIZE;
    const Bytes frame = makeFrame(fragments * fm::MAX_FRAGMENT_PAYLOAD, 2);
    std::vector<std::uint16_t> parityIndices(fm::MAX_FEC_GROUP_PARITY);
    std::iota(parityIndices.begin(), parityIndices.end(), std::uint16_t{0});

    std::vector<std::uint16_t> order(fragments);
    std::iota(order.begin(), order.end(), std::uint16_t{0});
    std::mt19937 random(GENERATE(1U, 2U, 3U, 4U, 5U));
    std::ranges::shuffle(order, random);
    std::vector<std::uint16_t> missing(order.begin(), order.begin() + fm::MAX_FEC_GROUP_PARITY);

    Bytes damaged = frame;
    for (const std::uint16_t fragIndex : missing) {
        damageFragment(damaged, fragIndex);
    }
    REQUIRE(recoverWith(damaged, frame, missing, parityIndices));
    CHECK(damaged == frame);
}

TEST_CASE("the short last fragment is rebuilt without writing past the frame") {
    const Bytes frame = makeFrame(2 * fm::MAX_FRAGMENT_PAYLOAD + 7, 3);
    Bytes buffer = frame;
    damageFragment(buffer, 2);
    buffer.resize(frame.size() + 16, 0x5A);

    const std::span<std::uint8_t> frameData = std::span(buffer).first(frame.size());
    Bytes block = parityOf(frame, 0);
    const std::array<fm::ParityBlock, 1> parity{fm::ParityBlock{0, block}};
    const std::array<std::uint16_t, 1> missing{2};

    REQUIRE(fm::recoverFragments(frameData, missing, parity));
    CHECK(std::ranges::equal(frameData, frame));
    CHECK(std::ranges::all_of(std::span(buffer).subspan(frame.size()), [](std::uint8_t byte) { return byte == 0x5A; }));
}

TEST_CASE("a frame with several groups rebuilds each group from its own parity") {
    constexpr std::uint16_t fragments = 178;
    const std::uint16_t groups = fm::fecGroupCount(fragments);
    REQUIRE(groups == 6);
    const Bytes frame = makeFrame(fragments * fm::MAX_FRAGMENT_PAYLOAD - 321, 4);
    Bytes damaged = frame;

    for (std::uint16_t group = 0; group < groups; ++group) {
        const std::vector<std::uint16_t> missing{group, static_cast<std::uint16_t>(group + groups)};
        for (const std::uint16_t fragIndex : missing) {
            damageFragment(damaged, fragIndex);
        }
        const std::vector<std::uint16_t> parityIndices{group, static_cast<std::uint16_t>(group + 2 * groups)};
        REQUIRE(recoverWith(damaged, frame, missing, parityIndices));
    }
    CHECK(damaged == frame);
}

TEST_CASE("a frame of one fragment is rebuilt from either parity packet") {
    const Bytes frame = makeFrame(300, 5);
    const std::uint16_t parityIndex = GENERATE(std::uint16_t{0}, std::uint16_t{1});
    Bytes damaged = frame;
    damageFragment(damaged, 0);
    REQUIRE(recoverWith(damaged, frame, {0}, {parityIndex}));
    CHECK(damaged == frame);
}

TEST_CASE("recovery refuses inputs that cannot work and leaves the frame alone") {
    const Bytes frame = makeFrame(40 * fm::MAX_FRAGMENT_PAYLOAD, 6);
    Bytes damaged = frame;

    SECTION("a different number of parity packets") {
        CHECK_FALSE(recoverWith(damaged, frame, {0, 2}, {0}));
    }
    SECTION("no lost fragments") {
        CHECK_FALSE(recoverWith(damaged, frame, {}, {}));
    }
    SECTION("fragments from different groups") {
        CHECK_FALSE(recoverWith(damaged, frame, {0, 1}, {0, 2}));
    }
    SECTION("parity from another group") {
        CHECK_FALSE(recoverWith(damaged, frame, {0}, {1}));
    }
    SECTION("a fragment past the end of the frame") {
        CHECK_FALSE(recoverWith(damaged, frame, {0, 40}, {0, 2}));
    }
    SECTION("a parity block of the wrong size") {
        Bytes block = parityOf(frame, 0);
        block.pop_back();
        const std::array<fm::ParityBlock, 1> parity{fm::ParityBlock{0, block}};
        const std::array<std::uint16_t, 1> missing{0};
        CHECK_FALSE(fm::recoverFragments(damaged, missing, parity));
    }
    CHECK(damaged == frame);
}

TEST_CASE("computing parity refuses a bad buffer, an empty frame or a row past the limit") {
    const Bytes frame = makeFrame(100, 7);
    Bytes wrongSize(99);
    CHECK_FALSE(fm::computeParity(frame, 0, wrongSize));
    Bytes parity(100);
    CHECK_FALSE(fm::computeParity(frame, fm::MAX_FEC_GROUP_PARITY, parity));
    Bytes none;
    CHECK_FALSE(fm::computeParity(none, 0, none));
}
