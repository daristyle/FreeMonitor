#include "ControlPackets.hpp"
#include "ControlTestHelpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
    constexpr std::uint32_t SESSION_ID = 7;
    constexpr std::size_t VERSION_OFFSET = 2;
    constexpr std::size_t TYPE_OFFSET = 3;
    constexpr std::size_t SESSION_ID_OFFSET = 4;
    constexpr std::size_t HEADER_RESERVED_OFFSET = 12;
    constexpr std::size_t MSG_ID_OFFSET = 24;
    constexpr std::size_t CONTROL_RESERVED_OFFSET = 28;

    std::vector<std::uint8_t> withByte(std::vector<std::uint8_t> packet, std::size_t offset, std::uint8_t value) {
        packet[offset] = value;
        return packet;
    }
}

TEST_CASE("every control message built by the helpers is valid") {
    const auto packet =
            GENERATE(helloPacket(SESSION_ID, 1, 1), welcomePacket(SESSION_ID, 1, 1), ackPacket(SESSION_ID, 1, 1),
                     heartbeatPacket(SESSION_ID, 1, 5, 4, 3), keyframeRequestPacket(SESSION_ID, 1),
                     statsPacket(SESSION_ID, 1, 10, 2), byePacket(SESSION_ID, 1), rejectPacket(SESSION_ID, 2));
    CHECK(fm::checkControlPacket(packet) == fm::ControlPacketCheck::VALID);
}

TEST_CASE("control packets that break a rule of the spec are invalid") {
    std::vector<std::uint8_t> packet;
    SECTION("wrong magic") {
        packet = withByte(ackPacket(SESSION_ID, 1, 1), 0, 0);
    }
    SECTION("sessionId 0") {
        packet = ackPacket(0, 1, 1);
    }
    SECTION("one byte too long") {
        packet = ackPacket(SESSION_ID, 1, 1);
        packet.push_back(0);
    }
    SECTION("one byte too short") {
        packet = ackPacket(SESSION_ID, 1, 1);
        packet.pop_back();
    }
    SECTION("headerReserved is not 0") {
        packet = withByte(ackPacket(SESSION_ID, 1, 1), HEADER_RESERVED_OFFSET, 1);
    }
    SECTION("controlReserved is not 0") {
        packet = withByte(ackPacket(SESSION_ID, 1, 1), CONTROL_RESERVED_OFFSET, 1);
    }
    SECTION("an unreliable message with a msgId") {
        packet = withByte(ackPacket(SESSION_ID, 1, 1), MSG_ID_OFFSET + 3, 1);
    }
    SECTION("the reserved field of HEARTBEAT is not 0") {
        packet = withByte(heartbeatPacket(SESSION_ID, 1, 5), fm::Heartbeat::SIZE - 1, 1);
    }
    SECTION("the reserved field of REJECT is not 0") {
        packet = withByte(rejectPacket(SESSION_ID, 1), fm::Reject::SIZE - 1, 1);
    }
    SECTION("HELLO with an unknown codec bit") {
        packet = helloPacket(SESSION_ID, 1, 1, 2944, 1840, BOTH_CODECS | 0x04);
    }
    SECTION("an unknown packet type") {
        packet = withByte(byePacket(SESSION_ID, 1), TYPE_OFFSET, 0x29);
    }
    SECTION("a video fragment is not a control message") {
        packet = withByte(byePacket(SESSION_ID, 1), TYPE_OFFSET,
                          static_cast<std::uint8_t>(fm::PacketType::VIDEO_FRAGMENT));
    }
    SECTION("an ACK of another protocol version") {
        packet = withByte(ackPacket(SESSION_ID, 1, 1), VERSION_OFFSET, 2);
    }
    SECTION("shorter than the common header") {
        packet = std::vector<std::uint8_t>(fm::CommonHeader::SIZE - 1, 0);
    }
    CHECK(fm::checkControlPacket(packet) == fm::ControlPacketCheck::INVALID);
}

TEST_CASE("a HELLO of another protocol version is recognised so the host can reject it") {
    CHECK(fm::checkControlPacket(withByte(helloPacket(SESSION_ID, 1, 1), VERSION_OFFSET, 2)) ==
          fm::ControlPacketCheck::UNSUPPORTED_VERSION);
    CHECK(fm::checkControlPacket(withByte(withByte(helloPacket(SESSION_ID, 1, 1), VERSION_OFFSET, 2),
                                          SESSION_ID_OFFSET + 3, 0)) == fm::ControlPacketCheck::INVALID);
}

TEST_CASE("a REJECT is valid whatever its protocol version, because it is frozen") {
    CHECK(fm::checkControlPacket(rejectPacket(SESSION_ID, 1, 9)) == fm::ControlPacketCheck::VALID);
}

TEST_CASE("restampHeader writes only seq and timestampUs") {
    auto packet = helloPacket(SESSION_ID, 1, 5);
    fm::restampHeader(packet, 0xAABBCCDD, 0x0102030405060708);
    fm::Hello hello;
    REQUIRE(hello.read(packet));
    CHECK(hello.commonHeader.seq == 0xAABBCCDD);
    CHECK(hello.commonHeader.timestampUs == 0x0102030405060708);
    CHECK(hello.commonHeader.sessionId == SESSION_ID);
    CHECK(hello.controlHeader.msgId == 5);
    CHECK(hello.width == 2944);
}

TEST_CASE("isMediaPacket accepts video and cursor packets with a valid header") {
    fm::CommonHeader header;
    header.type = static_cast<std::uint8_t>(fm::PacketType::VIDEO_FRAGMENT);
    CHECK(fm::isMediaPacket(header));
    header.type = static_cast<std::uint8_t>(fm::PacketType::VIDEO_PARITY);
    CHECK(fm::isMediaPacket(header));
    header.type = static_cast<std::uint8_t>(fm::PacketType::CURSOR_SHAPE);
    CHECK(fm::isMediaPacket(header));
    header.type = static_cast<std::uint8_t>(fm::PacketType::ACK);
    CHECK_FALSE(fm::isMediaPacket(header));
    header.type = static_cast<std::uint8_t>(fm::PacketType::VIDEO_FRAGMENT);
    header.headerReserved = 1;
    CHECK_FALSE(fm::isMediaPacket(header));
}

TEST_CASE("only HELLO and WELCOME are reliable") {
    CHECK(fm::isReliable(fm::PacketType::HELLO));
    CHECK(fm::isReliable(fm::PacketType::WELCOME));
    CHECK_FALSE(fm::isReliable(fm::PacketType::ACK));
    CHECK_FALSE(fm::isReliable(fm::PacketType::KEYFRAME_REQUEST));
}
