#include "Fragmenter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {
    using PacketBuffer = std::array<std::uint8_t, fm::MAX_PACKET_SIZE>;

    constexpr std::size_t FULL_PACKET_SIZE = fm::VideoFragment::SIZE + fm::MAX_FRAGMENT_PAYLOAD;
    constexpr std::uint32_t SESSION_ID = 0x11223344;
    constexpr std::uint64_t TIMESTAMP_US = 1000;

    std::vector<std::uint8_t> makeFrame(std::size_t size) {
        std::vector<std::uint8_t> frame(size);
        for (std::size_t index = 0; index < size; ++index) {
            frame[index] = static_cast<std::uint8_t>(index % 251);
        }
        return frame;
    }

    std::vector<std::uint8_t> joinPayloads(const fm::Fragmenter& fragmenter) {
        std::vector<std::uint8_t> joined;
        PacketBuffer packet{};
        for (std::uint16_t index = 0; index < fragmenter.fragmentCount(); ++index) {
            const std::size_t length = fragmenter.write(index, SESSION_ID, index, TIMESTAMP_US, packet);
            fm::VideoFragment fragment;
            if (length == 0 || !fragment.read(std::span(packet.data(), length))) {
                return {};
            }
            joined.insert(joined.end(), fragment.payload.begin(), fragment.payload.end());
        }
        return joined;
    }
}

TEST_CASE("the spec example: 50,000 bytes become 37 fragments") {
    const std::vector<std::uint8_t> frame = makeFrame(50'000);
    fm::Fragmenter fragmenter;
    REQUIRE(fragmenter.begin(frame, 1, fm::Codec::JPEG, true, 0));
    REQUIRE(fragmenter.fragmentCount() == 37);

    PacketBuffer packet{};
    CHECK(fragmenter.write(0, SESSION_ID, 0, TIMESTAMP_US, packet) == FULL_PACKET_SIZE);
    CHECK(fragmenter.write(35, SESSION_ID, 35, TIMESTAMP_US, packet) == FULL_PACKET_SIZE);
    CHECK(fragmenter.write(36, SESSION_ID, 36, TIMESTAMP_US, packet) == fm::VideoFragment::SIZE + 1328);
    CHECK(joinPayloads(fragmenter) == frame);
}

TEST_CASE("fragment counts and last fragment sizes at the boundaries") {
    struct Case {
        std::size_t frameSize;
        std::uint16_t fragmentCount;
        std::size_t lastPayloadSize;
    };
    constexpr std::array<Case, 5> cases{{
            {1, 1, 1},
            {fm::MAX_FRAGMENT_PAYLOAD, 1, fm::MAX_FRAGMENT_PAYLOAD},
            {fm::MAX_FRAGMENT_PAYLOAD + 1, 2, 1},
            {2 * fm::MAX_FRAGMENT_PAYLOAD, 2, fm::MAX_FRAGMENT_PAYLOAD},
            {2 * fm::MAX_FRAGMENT_PAYLOAD + 1, 3, 1},
    }};

    for (const Case& testCase : cases) {
        CAPTURE(testCase.frameSize);
        const std::vector<std::uint8_t> frame = makeFrame(testCase.frameSize);
        fm::Fragmenter fragmenter;
        REQUIRE(fragmenter.begin(frame, 1, fm::Codec::JPEG, true, 0));
        REQUIRE(fragmenter.fragmentCount() == testCase.fragmentCount);

        PacketBuffer packet{};
        const auto lastIndex = static_cast<std::uint16_t>(testCase.fragmentCount - 1);
        CHECK(fragmenter.write(lastIndex, SESSION_ID, 0, TIMESTAMP_US, packet) ==
              fm::VideoFragment::SIZE + testCase.lastPayloadSize);
        CHECK(joinPayloads(fragmenter) == frame);
    }
}

TEST_CASE("a frame of exactly MAX_FRAME_SIZE is accepted and survives the round trip") {
    const std::vector<std::uint8_t> frame = makeFrame(fm::MAX_FRAME_SIZE);
    fm::Fragmenter fragmenter;
    REQUIRE(fragmenter.begin(frame, 1, fm::Codec::H264, true, 0));
    CHECK(fragmenter.fragmentCount() == 3103);
    CHECK(joinPayloads(fragmenter) == frame);
}

TEST_CASE("empty and oversized frames are rejected") {
    const std::vector<std::uint8_t> valid = makeFrame(5000);
    const std::vector<std::uint8_t> oversized = makeFrame(fm::MAX_FRAME_SIZE + 1);
    fm::Fragmenter fragmenter;
    REQUIRE(fragmenter.begin(valid, 1, fm::Codec::JPEG, true, 0));

    SECTION("empty") {
        CHECK_FALSE(fragmenter.begin({}, 2, fm::Codec::JPEG, true, 0));
    }
    SECTION("one byte over MAX_FRAME_SIZE") {
        CHECK_FALSE(fragmenter.begin(oversized, 2, fm::Codec::JPEG, true, 0));
    }

    PacketBuffer packet{};
    CHECK(fragmenter.fragmentCount() == 0);
    CHECK(fragmenter.write(0, SESSION_ID, 0, TIMESTAMP_US, packet) == 0);
}

TEST_CASE("every header field is written as the spec defines it") {
    const std::vector<std::uint8_t> frame = makeFrame(3000);
    fm::Fragmenter fragmenter;
    REQUIRE(fragmenter.begin(frame, 0xFFFFFFFF, fm::Codec::H264, false, 0x0102030405060708));

    PacketBuffer packet{};
    const std::size_t length = fragmenter.write(1, 0xA1B2C3D4, 77, 0x1122334455667788, packet);
    REQUIRE(length == fm::VideoFragment::SIZE + fm::MAX_FRAGMENT_PAYLOAD);

    fm::VideoFragment fragment;
    REQUIRE(fragment.read(std::span(packet.data(), length)));
    CHECK(fragment.commonHeader.magic == fm::MAGIC);
    CHECK(fragment.commonHeader.version == fm::PROTOCOL_VERSION);
    CHECK(fragment.commonHeader.type == static_cast<std::uint8_t>(fm::PacketType::VIDEO_FRAGMENT));
    CHECK(fragment.commonHeader.sessionId == 0xA1B2C3D4);
    CHECK(fragment.commonHeader.seq == 77);
    CHECK(fragment.commonHeader.headerReserved == 0);
    CHECK(fragment.commonHeader.timestampUs == 0x1122334455667788);
    CHECK(fragment.frameId == 0xFFFFFFFF);
    CHECK(fragment.fragIndex == 1);
    CHECK(fragment.fragCount == 3);
    CHECK(fragment.frameSize == 3000);
    CHECK(fragment.codec == static_cast<std::uint8_t>(fm::Codec::H264));
    CHECK(fragment.flags == 0);
    CHECK(fragment.reserved == 0);
    CHECK(fragment.captureTimestampUs == 0x0102030405060708);
}

TEST_CASE("a keyframe sets only the KEYFRAME flag") {
    const std::vector<std::uint8_t> frame = makeFrame(100);
    fm::Fragmenter fragmenter;
    REQUIRE(fragmenter.begin(frame, 1, fm::Codec::H264, true, 0));

    PacketBuffer packet{};
    const std::size_t length = fragmenter.write(0, SESSION_ID, 0, TIMESTAMP_US, packet);
    fm::VideoFragment fragment;
    REQUIRE(fragment.read(std::span(packet.data(), length)));
    CHECK(fragment.flags == fm::FragmentFlags::KEYFRAME);
}

TEST_CASE("an index past the end or a buffer that is too small writes nothing") {
    const std::vector<std::uint8_t> frame = makeFrame(2 * fm::MAX_FRAGMENT_PAYLOAD);
    fm::Fragmenter fragmenter;
    REQUIRE(fragmenter.begin(frame, 1, fm::Codec::JPEG, true, 0));

    PacketBuffer packet{};
    CHECK(fragmenter.write(2, SESSION_ID, 0, TIMESTAMP_US, packet) == 0);
    CHECK(fragmenter.write(0, SESSION_ID, 0, TIMESTAMP_US, std::span(packet.data(), FULL_PACKET_SIZE - 1)) == 0);
    CHECK(fragmenter.write(0, SESSION_ID, 0, TIMESTAMP_US, std::span(packet.data(), FULL_PACKET_SIZE)) ==
          FULL_PACKET_SIZE);
}
