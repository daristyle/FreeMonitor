#include "ControlTestHelpers.hpp"
#include "HeartbeatEcho.hpp"
#include "PeerTracker.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the peer tracker follows a new address only for packets newer than every seq so far") {
    fm::PeerTracker tracker;
    tracker.reset(TABLET_ADDRESS, START);
    tracker.notePacket(TABLET_ADDRESS, 100, 40, START);
    tracker.notePacket(OTHER_TABLET_ADDRESS, 105, 40, START);
    CHECK(tracker.address() == OTHER_TABLET_ADDRESS);
    tracker.notePacket(TABLET_ADDRESS, 103, 40, START);
    CHECK(tracker.address() == OTHER_TABLET_ADDRESS);
}

TEST_CASE("the peer tracker counts gaps in seq as lost and takes back late arrivals") {
    fm::PeerTracker tracker;
    tracker.reset(TABLET_ADDRESS, START);
    tracker.notePacket(TABLET_ADDRESS, 10, 100, START);
    tracker.notePacket(TABLET_ADDRESS, 14, 100, START);
    CHECK(tracker.lost() == 3);
    tracker.notePacket(TABLET_ADDRESS, 12, 100, START);
    CHECK(tracker.lost() == 2);
    tracker.notePacket(TABLET_ADDRESS, 13, 100, START);
    CHECK(tracker.lost() == 1);
    CHECK(tracker.received() == 4);
    CHECK(tracker.bytes() == 400);
}

TEST_CASE("the peer tracker does not count a duplicate as lost or found") {
    fm::PeerTracker tracker;
    tracker.reset(TABLET_ADDRESS, START);
    tracker.notePacket(TABLET_ADDRESS, 10, 1, START);
    tracker.notePacket(TABLET_ADDRESS, 12, 1, START);
    tracker.notePacket(TABLET_ADDRESS, 12, 1, START);
    CHECK(tracker.lost() == 1);
}

TEST_CASE("the peer tracker handles seq wrapping from 2^32 - 1 to 0") {
    fm::PeerTracker tracker;
    tracker.reset(TABLET_ADDRESS, START);
    tracker.notePacket(TABLET_ADDRESS, 0xFFFF'FFFE, 1, START);
    tracker.notePacket(TABLET_ADDRESS, 1, 1, START);
    CHECK(tracker.lost() == 2);
    tracker.notePacket(OTHER_TABLET_ADDRESS, 2, 1, START);
    CHECK(tracker.address() == OTHER_TABLET_ADDRESS);
}

TEST_CASE("the peer tracker reports silence from the last packet on") {
    fm::PeerTracker tracker;
    tracker.reset(TABLET_ADDRESS, START);
    tracker.notePacket(TABLET_ADDRESS, 1, 1, START + 1s);
    CHECK_FALSE(tracker.silentFor(START + 3999ms, 3000ms));
    CHECK(tracker.silentFor(START + 4s, 3000ms));
}

TEST_CASE("the heartbeat echo returns the peer's timestamp with the delay and measures the round trip") {
    fm::HeartbeatEcho echo;
    const fm::Heartbeat first = echo.nextHeartbeat(START);
    CHECK(first.echoTimestampUs == 0);
    CHECK(first.echoDelayUs == 0);

    fm::Heartbeat fromPeer;
    fromPeer.commonHeader.timestampUs = 555;
    fromPeer.echoTimestampUs = timestampOf(START);
    fromPeer.echoDelayUs = 30'000;
    echo.received(fromPeer, START + 100ms);
    CHECK(echo.roundTripTime() == std::chrono::microseconds(70'000));

    const fm::Heartbeat next = echo.nextHeartbeat(START + 500ms);
    CHECK(next.echoTimestampUs == 555);
    CHECK(next.echoDelayUs == 400'000);
}

TEST_CASE("a heartbeat without an echo leaves the round-trip time unknown") {
    fm::HeartbeatEcho echo;
    fm::Heartbeat fromPeer;
    fromPeer.commonHeader.timestampUs = 555;
    echo.received(fromPeer, START);
    CHECK_FALSE(echo.roundTripTime());
}
