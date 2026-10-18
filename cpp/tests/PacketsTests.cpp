#include "Packets.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
    using PacketBuffer = std::array<std::uint8_t, fm::MAX_PACKET_SIZE>;

    fm::CommonHeader sampleCommonHeader(fm::PacketType type) {
        fm::CommonHeader header = fm::makeCommonHeader(type);
        header.sessionId = 0xA1B2C3D4;
        header.seq = 0x01020304;
        header.timestampUs = 0x1122334455667788;
        return header;
    }

    fm::ControlHeader sampleControlHeader() {
        fm::ControlHeader header;
        header.msgId = 0x0A0B0C0D;
        return header;
    }

    void checkCommonHeader(const fm::CommonHeader& actual, const fm::CommonHeader& expected) {
        CHECK(actual.magic == expected.magic);
        CHECK(actual.version == expected.version);
        CHECK(actual.type == expected.type);
        CHECK(actual.sessionId == expected.sessionId);
        CHECK(actual.seq == expected.seq);
        CHECK(actual.headerReserved == expected.headerReserved);
        CHECK(actual.timestampUs == expected.timestampUs);
    }

    void checkControlHeader(const fm::ControlHeader& actual, const fm::ControlHeader& expected) {
        CHECK(actual.msgId == expected.msgId);
        CHECK(actual.controlReserved == expected.controlReserved);
    }

    template<typename Packet>
    Packet roundTrip(const Packet& packet, std::span<std::uint8_t> buffer, std::size_t expectedLength) {
        REQUIRE(packet.write(buffer) == expectedLength);
        Packet result;
        REQUIRE(result.read(buffer.first(expectedLength)));
        return result;
    }

    template<std::size_t N>
    bool startsWith(const PacketBuffer& buffer, const std::array<std::uint8_t, N>& expected) {
        return std::ranges::equal(std::span(buffer).first(N), expected);
    }
}

TEST_CASE("packet sizes match the spec") {
    STATIC_REQUIRE(fm::CommonHeader::SIZE == 24);
    STATIC_REQUIRE(fm::ControlHeader::OFFSET == 24);
    STATIC_REQUIRE(fm::ControlHeader::SIZE == 8);
    STATIC_REQUIRE(fm::VideoFragment::SIZE == 48);
    STATIC_REQUIRE(fm::CursorPos::SIZE == 40);
    STATIC_REQUIRE(fm::CursorShape::SIZE == 40);
    STATIC_REQUIRE(fm::Hello::SIZE == 40);
    STATIC_REQUIRE(fm::Welcome::SIZE == 40);
    STATIC_REQUIRE(fm::ACK::SIZE == 36);
    STATIC_REQUIRE(fm::Heartbeat::SIZE == 48);
    STATIC_REQUIRE(fm::KeyframeRequest::SIZE == 32);
    STATIC_REQUIRE(fm::ShapeRequest::SIZE == 36);
    STATIC_REQUIRE(fm::Stats::SIZE == 52);
    STATIC_REQUIRE(fm::Bye::SIZE == 32);
    STATIC_REQUIRE(fm::Reject::SIZE == 36);
    STATIC_REQUIRE(fm::MAX_FRAGMENT_PAYLOAD == 1352);
    STATIC_REQUIRE(fm::MAX_FRAGMENT_PAYLOAD == fm::MAX_PACKET_SIZE - fm::VideoFragment::SIZE);
    STATIC_REQUIRE(fm::MAX_SHAPE_PAYLOAD == fm::MAX_PACKET_SIZE - fm::CursorShape::SIZE);
}

TEST_CASE("default packets carry the spec's magic, version and their own type") {
    const auto checkDefaults = [](const fm::CommonHeader& header, fm::PacketType type) {
        CHECK(header.magic == 0x464D);
        CHECK(header.version == 1);
        CHECK(header.type == static_cast<std::uint8_t>(type));
        CHECK(header.headerReserved == 0);
    };
    checkDefaults(fm::VideoFragment{}.commonHeader, fm::PacketType::VIDEO_FRAGMENT);
    checkDefaults(fm::CursorPos{}.commonHeader, fm::PacketType::CURSOR_POS);
    checkDefaults(fm::CursorShape{}.commonHeader, fm::PacketType::CURSOR_SHAPE);
    checkDefaults(fm::Hello{}.commonHeader, fm::PacketType::HELLO);
    checkDefaults(fm::Welcome{}.commonHeader, fm::PacketType::WELCOME);
    checkDefaults(fm::ACK{}.commonHeader, fm::PacketType::ACK);
    checkDefaults(fm::Heartbeat{}.commonHeader, fm::PacketType::HEARTBEAT);
    checkDefaults(fm::KeyframeRequest{}.commonHeader, fm::PacketType::KEYFRAME_REQUEST);
    checkDefaults(fm::ShapeRequest{}.commonHeader, fm::PacketType::SHAPE_REQUEST);
    checkDefaults(fm::Stats{}.commonHeader, fm::PacketType::STATS);
    checkDefaults(fm::Bye{}.commonHeader, fm::PacketType::BYE);
    checkDefaults(fm::Reject{}.commonHeader, fm::PacketType::REJECT);
}

TEST_CASE("common and control header bytes match the spec's offsets") {
    fm::KeyframeRequest packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::KEYFRAME_REQUEST);
    packet.controlHeader = sampleControlHeader();

    PacketBuffer buffer{};
    REQUIRE(packet.write(buffer) == 32);
    constexpr std::array<std::uint8_t, 32> expected{
            0x46, 0x4D, 0x01, 0x24, 0xA1, 0xB2, 0xC3, 0xD4, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK(startsWith(buffer, expected));
}

TEST_CASE("VIDEO_FRAGMENT bytes match the spec's offsets") {
    constexpr std::array<std::uint8_t, 2> payload{0xEE, 0xFF};
    fm::VideoFragment packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::VIDEO_FRAGMENT);
    packet.frameId = 0xDEADBEEF;
    packet.fragIndex = 0x0102;
    packet.fragCount = 0x0304;
    packet.frameSize = 0x00050607;
    packet.codec = static_cast<std::uint8_t>(fm::Codec::H264);
    packet.flags = fm::FragmentFlags::KEYFRAME;
    packet.captureTimestampUs = 0x0102030405060708;
    packet.payload = payload;

    PacketBuffer buffer{};
    REQUIRE(packet.write(buffer) == 50);
    constexpr std::array<std::uint8_t, 50> expected{
            0x46, 0x4D, 0x01, 0x01, 0xA1, 0xB2, 0xC3, 0xD4, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x11,
            0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x00, 0x05,
            0x06, 0x07, 0x01, 0x01, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xEE, 0xFF,
    };
    CHECK(startsWith(buffer, expected));
}

TEST_CASE("HELLO puts codecs at byte 38 and features at byte 39") {
    fm::Hello packet;
    packet.width = 0x0780;
    packet.height = 0x04B0;
    packet.refreshHz = 0x003C;
    packet.codecs = static_cast<std::uint8_t>(fm::CodecFlags::JPEG | fm::CodecFlags::H264);
    packet.features = fm::FeatureFlags::CURSOR_OVERLAY;

    PacketBuffer buffer{};
    REQUIRE(packet.write(buffer) == 40);
    CHECK(buffer[32] == 0x07);
    CHECK(buffer[33] == 0x80);
    CHECK(buffer[34] == 0x04);
    CHECK(buffer[35] == 0xB0);
    CHECK(buffer[36] == 0x00);
    CHECK(buffer[37] == 0x3C);
    CHECK(buffer[38] == 0x03);
    CHECK(buffer[39] == 0x01);
}

TEST_CASE("STATS puts bytesReceived at bytes 48 to 51") {
    fm::Stats packet;
    packet.framesDropped = 0x0A0B0C0D;
    packet.bytesReceived = 0x01020304;

    PacketBuffer buffer{};
    REQUIRE(packet.write(buffer) == 52);
    constexpr std::array<std::uint8_t, 8> expected{0x0A, 0x0B, 0x0C, 0x0D, 0x01, 0x02, 0x03, 0x04};
    CHECK(std::ranges::equal(std::span(buffer).subspan(44, 8), expected));
}

TEST_CASE("CURSOR_POS writes signed coordinates in two's complement") {
    fm::CursorPos packet;
    packet.x = -1;
    packet.y = -2;

    PacketBuffer buffer{};
    REQUIRE(packet.write(buffer) == 40);
    constexpr std::array<std::uint8_t, 8> expected{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    CHECK(std::ranges::equal(std::span(buffer).subspan(28, 8), expected));

    fm::CursorPos result;
    REQUIRE(result.read(std::span(buffer).first(40)));
    CHECK(result.x == -1);
    CHECK(result.y == -2);
}

TEST_CASE("VIDEO_FRAGMENT round trip") {
    constexpr std::array<std::uint8_t, 5> payload{1, 2, 3, 4, 5};
    fm::VideoFragment packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::VIDEO_FRAGMENT);
    packet.frameId = 0xDEADBEEF;
    packet.fragIndex = 0x0102;
    packet.fragCount = 0x0304;
    packet.frameSize = 0x00050607;
    packet.codec = static_cast<std::uint8_t>(fm::Codec::H264);
    packet.flags = fm::FragmentFlags::KEYFRAME;
    packet.captureTimestampUs = 0x0102030405060708;
    packet.payload = payload;

    PacketBuffer buffer{};
    const fm::VideoFragment result = roundTrip(packet, buffer, fm::VideoFragment::SIZE + payload.size());
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    CHECK(result.frameId == packet.frameId);
    CHECK(result.fragIndex == packet.fragIndex);
    CHECK(result.fragCount == packet.fragCount);
    CHECK(result.frameSize == packet.frameSize);
    CHECK(result.codec == packet.codec);
    CHECK(result.flags == packet.flags);
    CHECK(result.reserved == 0);
    CHECK(result.captureTimestampUs == packet.captureTimestampUs);
    CHECK(std::ranges::equal(result.payload, payload));
}

TEST_CASE("CURSOR_POS round trip") {
    fm::CursorPos packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::CURSOR_POS);
    packet.shapeId = 0x0BADF00D;
    packet.x = -1234;
    packet.y = 5678;
    packet.flags = fm::CursorFlags::VISIBLE;

    PacketBuffer buffer{};
    const fm::CursorPos result = roundTrip(packet, buffer, fm::CursorPos::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    CHECK(result.shapeId == packet.shapeId);
    CHECK(result.x == packet.x);
    CHECK(result.y == packet.y);
    CHECK(result.flags == packet.flags);
    CHECK(result.reserved0 == 0);
    CHECK(result.reserved1 == 0);
}

TEST_CASE("CURSOR_SHAPE round trip") {
    std::array<std::uint8_t, 16> payload{};
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(0x80 + index);
    }
    fm::CursorShape packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::CURSOR_SHAPE);
    packet.shapeId = 0x01020304;
    packet.fragIndex = 2;
    packet.fragCount = 3;
    packet.width = 32;
    packet.height = 48;
    packet.hotspotX = 5;
    packet.hotspotY = 7;
    packet.payload = payload;

    PacketBuffer buffer{};
    const fm::CursorShape result = roundTrip(packet, buffer, fm::CursorShape::SIZE + payload.size());
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    CHECK(result.shapeId == packet.shapeId);
    CHECK(result.fragIndex == packet.fragIndex);
    CHECK(result.fragCount == packet.fragCount);
    CHECK(result.width == packet.width);
    CHECK(result.height == packet.height);
    CHECK(result.hotspotX == packet.hotspotX);
    CHECK(result.hotspotY == packet.hotspotY);
    CHECK(std::ranges::equal(result.payload, payload));
}

TEST_CASE("HELLO round trip") {
    fm::Hello packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::HELLO);
    packet.controlHeader = sampleControlHeader();
    packet.width = 1920;
    packet.height = 1080;
    packet.refreshHz = 60;
    packet.codecs = static_cast<std::uint8_t>(fm::CodecFlags::JPEG | fm::CodecFlags::H264);
    packet.features = fm::FeatureFlags::CURSOR_OVERLAY;

    PacketBuffer buffer{};
    const fm::Hello result = roundTrip(packet, buffer, fm::Hello::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
    CHECK(result.width == packet.width);
    CHECK(result.height == packet.height);
    CHECK(result.refreshHz == packet.refreshHz);
    CHECK(result.codecs == packet.codecs);
    CHECK(result.features == packet.features);
}

TEST_CASE("WELCOME round trip") {
    fm::Welcome packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::WELCOME);
    packet.controlHeader = sampleControlHeader();
    packet.width = 2560;
    packet.height = 1600;
    packet.fps = 120;
    packet.codec = static_cast<std::uint8_t>(fm::Codec::H264);
    packet.features = fm::FeatureFlags::CURSOR_OVERLAY;

    PacketBuffer buffer{};
    const fm::Welcome result = roundTrip(packet, buffer, fm::Welcome::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
    CHECK(result.width == packet.width);
    CHECK(result.height == packet.height);
    CHECK(result.fps == packet.fps);
    CHECK(result.codec == packet.codec);
    CHECK(result.features == packet.features);
}

TEST_CASE("ACK round trip") {
    fm::ACK packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::ACK);
    packet.controlHeader = sampleControlHeader();
    packet.ackedMsgId = 0x31323334;

    PacketBuffer buffer{};
    const fm::ACK result = roundTrip(packet, buffer, fm::ACK::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
    CHECK(result.ackedMsgId == packet.ackedMsgId);
}

TEST_CASE("HEARTBEAT round trip") {
    fm::Heartbeat packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::HEARTBEAT);
    packet.controlHeader = sampleControlHeader();
    packet.echoTimestampUs = 0x0F0E0D0C0B0A0908;
    packet.echoDelayUs = 4321;

    PacketBuffer buffer{};
    const fm::Heartbeat result = roundTrip(packet, buffer, fm::Heartbeat::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
    CHECK(result.echoTimestampUs == packet.echoTimestampUs);
    CHECK(result.echoDelayUs == packet.echoDelayUs);
    CHECK(result.reserved == 0);
}

TEST_CASE("KEYFRAME_REQUEST round trip") {
    fm::KeyframeRequest packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::KEYFRAME_REQUEST);
    packet.controlHeader = sampleControlHeader();

    PacketBuffer buffer{};
    const fm::KeyframeRequest result = roundTrip(packet, buffer, fm::KeyframeRequest::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
}

TEST_CASE("SHAPE_REQUEST round trip") {
    fm::ShapeRequest packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::SHAPE_REQUEST);
    packet.controlHeader = sampleControlHeader();
    packet.shapeId = 0x41424344;

    PacketBuffer buffer{};
    const fm::ShapeRequest result = roundTrip(packet, buffer, fm::ShapeRequest::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
    CHECK(result.shapeId == packet.shapeId);
}

TEST_CASE("STATS round trip") {
    fm::Stats packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::STATS);
    packet.controlHeader = sampleControlHeader();
    packet.packetsReceived = 100'000;
    packet.packetsLost = 250;
    packet.framesCompleted = 5999;
    packet.framesDropped = 12;
    packet.bytesReceived = 0xFFFFFFFF;

    PacketBuffer buffer{};
    const fm::Stats result = roundTrip(packet, buffer, fm::Stats::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
    CHECK(result.packetsReceived == packet.packetsReceived);
    CHECK(result.packetsLost == packet.packetsLost);
    CHECK(result.framesCompleted == packet.framesCompleted);
    CHECK(result.framesDropped == packet.framesDropped);
    CHECK(result.bytesReceived == packet.bytesReceived);
}

TEST_CASE("BYE round trip") {
    fm::Bye packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::BYE);
    packet.controlHeader = sampleControlHeader();

    PacketBuffer buffer{};
    const fm::Bye result = roundTrip(packet, buffer, fm::Bye::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
}

TEST_CASE("REJECT round trip") {
    fm::Reject packet;
    packet.commonHeader = sampleCommonHeader(fm::PacketType::REJECT);
    packet.controlHeader = sampleControlHeader();
    packet.reason = static_cast<std::uint8_t>(fm::RejectReason::UNSUPPORTED_VERSION);
    packet.version = 2;

    PacketBuffer buffer{};
    const fm::Reject result = roundTrip(packet, buffer, fm::Reject::SIZE);
    checkCommonHeader(result.commonHeader, packet.commonHeader);
    checkControlHeader(result.controlHeader, packet.controlHeader);
    CHECK(result.reason == packet.reason);
    CHECK(result.version == packet.version);
    CHECK(result.reserved == 0);
}

TEMPLATE_TEST_CASE("a buffer one byte too short is neither written nor read", "", fm::VideoFragment, fm::CursorPos,
                   fm::CursorShape, fm::Hello, fm::Welcome, fm::ACK, fm::Heartbeat, fm::KeyframeRequest,
                   fm::ShapeRequest, fm::Stats, fm::Bye, fm::Reject) {
    TestType packet;
    std::array<std::uint8_t, TestType::SIZE> buffer{};
    const std::span<std::uint8_t> exact(buffer);

    CHECK(packet.write(exact.first(TestType::SIZE - 1)) == 0);
    CHECK(packet.write(exact) == TestType::SIZE);
    CHECK_FALSE(packet.read(exact.first(TestType::SIZE - 1)));
    CHECK(packet.read(exact));
}

TEST_CASE("a payload that does not fit is not written") {
    constexpr std::array<std::uint8_t, 10> payload{};
    fm::VideoFragment packet;
    packet.payload = payload;

    PacketBuffer buffer{};
    CHECK(packet.write(std::span(buffer).first(fm::VideoFragment::SIZE + payload.size() - 1)) == 0);
    CHECK(packet.write(std::span(buffer).first(fm::VideoFragment::SIZE + payload.size())) ==
          fm::VideoFragment::SIZE + payload.size());
}

TEST_CASE("the header-only readers need the whole header") {
    std::array<std::uint8_t, fm::ControlHeader::OFFSET + fm::ControlHeader::SIZE> buffer{};
    const std::span<const std::uint8_t> bytes(buffer);
    fm::CommonHeader commonHeader;
    fm::ControlHeader controlHeader;

    CHECK_FALSE(commonHeader.read(bytes.first(fm::CommonHeader::SIZE - 1)));
    CHECK(commonHeader.read(bytes.first(fm::CommonHeader::SIZE)));
    CHECK_FALSE(controlHeader.read(bytes.first(buffer.size() - 1)));
    CHECK(controlHeader.read(bytes));
}
