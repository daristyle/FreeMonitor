#include "ControlTestHelpers.hpp"
#include "ReliableDelivery.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace {
    constexpr std::chrono::milliseconds RESEND_INTERVAL{100};

    std::uint32_t msgIdOf(std::span<const std::uint8_t> packet) {
        fm::ControlHeader controlHeader;
        controlHeader.read(packet);
        return controlHeader.msgId;
    }
}

TEST_CASE("a reliable message gets the next msgId and is due at once") {
    fm::ReliableSender sender;
    CHECK(sender.enqueue(helloPacket(7, 1, 0), START) == 1U);
    CHECK(sender.enqueue(helloPacket(7, 1, 0), START) == 2U);
    const auto due = sender.due(START, RESEND_INTERVAL);
    REQUIRE_FALSE(due.empty());
    CHECK(msgIdOf(due) == 1);
}

TEST_CASE("an unacknowledged message is resent once per interval, not more often") {
    fm::ReliableSender sender;
    sender.enqueue(helloPacket(7, 1, 0), START);
    CHECK_FALSE(sender.due(START, RESEND_INTERVAL).empty());
    CHECK(sender.due(START + 99ms, RESEND_INTERVAL).empty());
    CHECK_FALSE(sender.due(START + 100ms, RESEND_INTERVAL).empty());
    CHECK(sender.due(START + 150ms, RESEND_INTERVAL).empty());
    CHECK_FALSE(sender.due(START + 200ms, RESEND_INTERVAL).empty());
}

TEST_CASE("an ACK removes the message, and the next one is due at once") {
    fm::ReliableSender sender;
    sender.enqueue(helloPacket(7, 1, 0), START);
    sender.enqueue(welcomePacket(7, 1, 0), START);
    REQUIRE_FALSE(sender.due(START, RESEND_INTERVAL).empty());
    CHECK_FALSE(sender.acknowledge(2, START + 10ms));
    CHECK(sender.acknowledge(1, START + 10ms));
    const auto next = sender.due(START + 10ms, RESEND_INTERVAL);
    REQUIRE_FALSE(next.empty());
    CHECK(msgIdOf(next) == 2);
    CHECK(sender.acknowledge(2, START + 20ms));
    CHECK(sender.empty());
    CHECK(sender.due(START + 500ms, RESEND_INTERVAL).empty());
}

TEST_CASE("a duplicate ACK of an already removed message changes nothing") {
    fm::ReliableSender sender;
    sender.enqueue(helloPacket(7, 1, 0), START);
    sender.enqueue(welcomePacket(7, 1, 0), START);
    CHECK(sender.acknowledge(1, START));
    CHECK_FALSE(sender.acknowledge(1, START));
    CHECK_FALSE(sender.empty());
}

TEST_CASE("a full queue refuses more messages, and reset starts again at msgId 1") {
    fm::ReliableSender sender;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(sender.enqueue(helloPacket(7, 1, 0), START));
    }
    CHECK_FALSE(sender.enqueue(helloPacket(7, 1, 0), START));
    sender.reset();
    CHECK(sender.empty());
    CHECK(sender.enqueue(helloPacket(7, 1, 0), START) == 1U);
}

TEST_CASE("packets that are too short or too long for a control message are refused") {
    fm::ReliableSender sender;
    CHECK_FALSE(sender.enqueue(std::vector<std::uint8_t>(31, 0), START));
    CHECK_FALSE(sender.enqueue(std::vector<std::uint8_t>(fm::MAX_CONTROL_PACKET_SIZE + 1, 0), START));
}

TEST_CASE("the receiver processes each msgId once and only newer ones") {
    fm::ReliableReceiver receiver;
    CHECK(receiver.shouldProcess(1));
    CHECK_FALSE(receiver.shouldProcess(1));
    CHECK(receiver.shouldProcess(2));
    CHECK_FALSE(receiver.shouldProcess(1));
    receiver.reset();
    CHECK(receiver.shouldProcess(1));
}

TEST_CASE("the receiver compares msgIds with serial number arithmetic across the wrap") {
    fm::ReliableReceiver receiver;
    CHECK(receiver.shouldProcess(0xFFFF'FFFF));
    CHECK(receiver.shouldProcess(1));
    CHECK_FALSE(receiver.shouldProcess(0xFFFF'FFFF));
}
