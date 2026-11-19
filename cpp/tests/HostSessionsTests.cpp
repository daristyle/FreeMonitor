#include "ControlTestHelpers.hpp"
#include "HostSessions.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace {
    constexpr std::uint32_t TABLET_SESSION = 7;
    constexpr std::uint32_t OTHER_SESSION = 9;
    constexpr std::uint8_t NO_FEATURES = 0;
    constexpr fm::HostOffer OFFER{.width = 2944, .height = 1840, .fps = 60, .codecs = BOTH_CODECS, .features = 0};

    struct HostFixture {
        FakeControlTransport transport;
        RecordingListener listener;
        fm::HostSessions sessions;

        explicit HostFixture(fm::HostOffer offer = OFFER, std::size_t maxSessions = 1)
            : sessions(transport, listener, offer, counterFrom(5000), maxSessions) {}

        void establish(std::chrono::steady_clock::time_point now = START) {
            sessions.handlePacket(helloPacket(TABLET_SESSION, 100, 1), TABLET_ADDRESS, now);
            sessions.handlePacket(ackPacket(TABLET_SESSION, 101, 1), TABLET_ADDRESS, now);
            transport.sent.clear();
        }
    };
}

TEST_CASE("a HELLO is acknowledged and answered with a WELCOME for the best common codec") {
    const auto [tabletCodecs, expectedCodec] =
            GENERATE(std::pair{BOTH_CODECS, fm::Codec::H264}, std::pair{fm::CodecFlags::JPEG, fm::Codec::JPEG});
    HostFixture host;
    host.sessions.handlePacket(helloPacket(TABLET_SESSION, 100, 1, 2944, 1840, tabletCodecs), TABLET_ADDRESS, START);

    REQUIRE(host.transport.sent.size() == 2);
    const SentPacket ackSent = host.transport.sent[0];
    const SentPacket welcomeSent = host.transport.sent[1];
    CHECK(ackSent.type() == fm::PacketType::ACK);
    CHECK(welcomeSent.type() == fm::PacketType::WELCOME);
    CHECK(ackSent.to == TABLET_ADDRESS);
    CHECK(welcomeSent.to == TABLET_ADDRESS);
    fm::ACK acknowledgement;
    acknowledgement.read(ackSent.bytes);
    CHECK(acknowledgement.ackedMsgId == 1);
    fm::Welcome welcome;
    welcome.read(welcomeSent.bytes);
    CHECK(welcome.commonHeader.sessionId == TABLET_SESSION);
    CHECK(welcome.controlHeader.msgId == 1);
    CHECK(welcome.width == 2944);
    CHECK(welcome.height == 1840);
    CHECK(welcome.fps == 60);
    CHECK(welcome.codec == static_cast<std::uint8_t>(expectedCodec));
    CHECK(welcomeSent.header().seq == ackSent.header().seq + 1);
    CHECK(host.listener.started == std::vector<std::uint32_t>{TABLET_SESSION});
    CHECK(host.sessions.streamingSession() == nullptr);
}

TEST_CASE("a session only gets the features both sides have") {
    const auto [offered, asked, expected] =
            GENERATE(std::tuple{fm::FeatureFlags::FEC, fm::FeatureFlags::FEC, fm::FeatureFlags::FEC},
                     std::tuple{fm::FeatureFlags::FEC, NO_FEATURES, NO_FEATURES},
                     std::tuple{NO_FEATURES, fm::FeatureFlags::FEC, NO_FEATURES});
    fm::HostOffer offer = OFFER;
    offer.features = offered;
    HostFixture host(offer);

    host.sessions.handlePacket(helloPacket(TABLET_SESSION, 100, 1, 2944, 1840, BOTH_CODECS, asked), TABLET_ADDRESS,
                               START);
    host.sessions.handlePacket(ackPacket(TABLET_SESSION, 101, 1), TABLET_ADDRESS, START);

    const fm::Session* session = host.sessions.streamingSession();
    REQUIRE(session != nullptr);
    CHECK(session->features == expected);
}

TEST_CASE("the WELCOME is resent every 100 ms with the same msgId and a new seq until it is acknowledged") {
    HostFixture host;
    host.sessions.handlePacket(helloPacket(TABLET_SESSION, 100, 1), TABLET_ADDRESS, START);
    const std::uint32_t firstSeq = host.transport.ofType(fm::PacketType::WELCOME)[0].header().seq;
    host.transport.sent.clear();

    host.sessions.poll(START + 99ms);
    CHECK(host.transport.ofType(fm::PacketType::WELCOME).empty());
    host.sessions.poll(START + 100ms);
    host.sessions.poll(START + 200ms);
    const auto resent = host.transport.ofType(fm::PacketType::WELCOME);
    REQUIRE(resent.size() == 2);
    CHECK(resent[0].controlHeader().msgId == 1);
    CHECK(resent[1].controlHeader().msgId == 1);
    CHECK(resent[0].header().seq == firstSeq + 1);
    CHECK(resent[1].header().seq == firstSeq + 2);
    CHECK(resent[1].header().timestampUs == timestampOf(START + 200ms));

    host.sessions.handlePacket(ackPacket(TABLET_SESSION, 101, 1), TABLET_ADDRESS, START + 250ms);
    host.transport.sent.clear();
    host.sessions.poll(START + 400ms);
    CHECK(host.transport.ofType(fm::PacketType::WELCOME).empty());
}

TEST_CASE("streaming starts only with the ACK for the WELCOME, and with a keyframe") {
    HostFixture host;
    host.sessions.handlePacket(helloPacket(TABLET_SESSION, 100, 1), TABLET_ADDRESS, START);
    host.sessions.handlePacket(ackPacket(TABLET_SESSION, 101, 2), TABLET_ADDRESS, START);
    CHECK(host.sessions.streamingSession() == nullptr);

    host.sessions.handlePacket(ackPacket(TABLET_SESSION, 102, 1), TABLET_ADDRESS, START);
    const fm::Session* session = host.sessions.streamingSession();
    REQUIRE(session != nullptr);
    CHECK(session->sessionId == TABLET_SESSION);
    CHECK(session->peer == TABLET_ADDRESS);
    CHECK(host.listener.streaming == std::vector<std::uint32_t>{TABLET_SESSION});
    CHECK(host.sessions.takeKeyframeRequest());
    CHECK_FALSE(host.sessions.takeKeyframeRequest());
}

TEST_CASE("a repeated HELLO of the current session is only acknowledged") {
    HostFixture host;
    host.sessions.handlePacket(helloPacket(TABLET_SESSION, 100, 1), TABLET_ADDRESS, START);
    host.transport.sent.clear();
    host.sessions.handlePacket(helloPacket(TABLET_SESSION, 101, 1), TABLET_ADDRESS, START + 10ms);
    REQUIRE(host.transport.sent.size() == 1);
    CHECK(host.transport.sent[0].type() == fm::PacketType::ACK);
    CHECK(host.listener.started.size() == 1);
}

TEST_CASE("the host sends a heartbeat every 500 ms that echoes the tablet's and measures the round trip") {
    HostFixture host;
    host.establish();
    host.sessions.poll(START + 499ms);
    CHECK(host.transport.ofType(fm::PacketType::HEARTBEAT).empty());
    host.sessions.poll(START + 500ms);
    const auto first = host.transport.ofType(fm::PacketType::HEARTBEAT);
    REQUIRE(first.size() == 1);

    host.sessions.handlePacket(heartbeatPacket(TABLET_SESSION, 102, 777, first[0].header().timestampUs, 30'000),
                               TABLET_ADDRESS, START + 600ms);
    const auto report = host.sessions.report();
    REQUIRE(report);
    REQUIRE(report->clockSample);
    CHECK(report->clockSample->receivedTime == START + 600ms);
    CHECK(report->clockSample->roundTripTime == std::chrono::microseconds(70'000));

    host.sessions.poll(START + 1000ms);
    const auto second = host.transport.ofType(fm::PacketType::HEARTBEAT);
    REQUIRE(second.size() == 2);
    fm::Heartbeat heartbeat;
    heartbeat.read(second[1].bytes);
    CHECK(heartbeat.echoTimestampUs == 777);
    CHECK(heartbeat.echoDelayUs == 400'000);
}

TEST_CASE("keyframe requests are honoured, except within 100 ms of a keyframe") {
    HostFixture host;
    host.establish();
    REQUIRE(host.sessions.takeKeyframeRequest());
    host.sessions.keyframeSent(START);

    host.sessions.handlePacket(keyframeRequestPacket(TABLET_SESSION, 102), TABLET_ADDRESS, START + 50ms);
    CHECK_FALSE(host.sessions.takeKeyframeRequest());
    host.sessions.handlePacket(keyframeRequestPacket(TABLET_SESSION, 103), TABLET_ADDRESS, START + 150ms);
    CHECK(host.sessions.takeKeyframeRequest());
}

TEST_CASE("the last STATS of the tablet are in the report") {
    HostFixture host;
    host.establish();
    host.sessions.handlePacket(statsPacket(TABLET_SESSION, 102, 1234, 56), TABLET_ADDRESS, START + 1s);
    const auto report = host.sessions.report();
    REQUIRE(report);
    REQUIRE(report->stats);
    CHECK(report->stats->packetsReceived == 1234);
    CHECK(report->stats->packetsLost == 56);
}

TEST_CASE("a tablet that sends nothing for 3 s times out, and the host stops streaming without a BYE") {
    HostFixture host;
    host.establish();
    host.sessions.handlePacket(heartbeatPacket(TABLET_SESSION, 102, 5), TABLET_ADDRESS, START + 1s);
    host.sessions.poll(START + 3999ms);
    REQUIRE(host.sessions.streamingSession() != nullptr);
    host.sessions.poll(START + 4s);
    CHECK(host.sessions.streamingSession() == nullptr);
    CHECK(host.listener.ended == std::vector{std::pair{TABLET_SESSION, fm::SessionEndReason::TIMEOUT}});
    CHECK(host.transport.ofType(fm::PacketType::BYE).empty());
}

TEST_CASE("with two sessions, the timeout of the first does not skip the second in the same poll") {
    HostFixture host(OFFER, 2);
    host.establish();
    host.sessions.handlePacket(helloPacket(OTHER_SESSION, 50, 1), OTHER_TABLET_ADDRESS, START);
    host.sessions.handlePacket(ackPacket(OTHER_SESSION, 51, 1), OTHER_TABLET_ADDRESS, START);
    host.sessions.handlePacket(heartbeatPacket(OTHER_SESSION, 52, 5), OTHER_TABLET_ADDRESS, START + 2s);
    host.transport.sent.clear();

    host.sessions.poll(START + 3s);

    CHECK(host.listener.ended == std::vector{std::pair{TABLET_SESSION, fm::SessionEndReason::TIMEOUT}});
    REQUIRE(host.sessions.streamingSession() != nullptr);
    CHECK(host.sessions.streamingSession()->sessionId == OTHER_SESSION);
    const auto heartbeats = host.transport.ofType(fm::PacketType::HEARTBEAT);
    REQUIRE(heartbeats.size() == 1);
    CHECK(heartbeats[0].to == OTHER_TABLET_ADDRESS);
    CHECK(heartbeats[0].header().sessionId == OTHER_SESSION);
}

TEST_CASE("a BYE from the tablet ends the session at once") {
    HostFixture host;
    host.establish();
    host.sessions.handlePacket(byePacket(TABLET_SESSION, 102), TABLET_ADDRESS, START + 10ms);
    CHECK(host.sessions.streamingSession() == nullptr);
    CHECK(host.listener.ended == std::vector{std::pair{TABLET_SESSION, fm::SessionEndReason::BYE}});
}

TEST_CASE("a new tablet replaces the current one, which gets three BYEs 20 ms apart") {
    HostFixture host;
    host.establish();
    host.sessions.handlePacket(helloPacket(OTHER_SESSION, 50, 1), OTHER_TABLET_ADDRESS, START + 1s);
    host.sessions.poll(START + 1s + 10ms);
    host.sessions.poll(START + 1s + 20ms);
    host.sessions.poll(START + 1s + 40ms);
    host.sessions.poll(START + 1s + 60ms);

    const auto byes = host.transport.ofType(fm::PacketType::BYE);
    REQUIRE(byes.size() == 3);
    for (const SentPacket& bye : byes) {
        CHECK(bye.to == TABLET_ADDRESS);
        CHECK(bye.header().sessionId == TABLET_SESSION);
    }
    CHECK(host.listener.ended == std::vector{std::pair{TABLET_SESSION, fm::SessionEndReason::REPLACED}});
    CHECK(host.listener.started == std::vector<std::uint32_t>{TABLET_SESSION, OTHER_SESSION});
    const auto welcomes = host.transport.ofType(fm::PacketType::WELCOME);
    REQUIRE_FALSE(welcomes.empty());
    CHECK(welcomes[0].to == OTHER_TABLET_ADDRESS);
}

TEST_CASE("a HELLO the host cannot accept is answered with REJECT and the right reason") {
    std::vector<std::uint8_t> hello;
    fm::HostOffer offer = OFFER;
    fm::RejectReason expected = fm::RejectReason::INVALID_PARAMETERS;
    SECTION("width 0") {
        hello = helloPacket(TABLET_SESSION, 1, 1, 0, 1840);
    }
    SECTION("height 0") {
        hello = helloPacket(TABLET_SESSION, 1, 1, 2944, 0);
    }
    SECTION("no JPEG") {
        hello = helloPacket(TABLET_SESSION, 1, 1, 2944, 1840, fm::CodecFlags::H264);
    }
    SECTION("no codec in common") {
        offer.codecs = fm::CodecFlags::H264;
        hello = helloPacket(TABLET_SESSION, 1, 1, 2944, 1840, fm::CodecFlags::JPEG);
        expected = fm::RejectReason::NO_COMMON_CODEC;
    }
    SECTION("another protocol version") {
        hello = helloPacket(TABLET_SESSION, 1, 1);
        hello[2] = 2;
        expected = fm::RejectReason::UNSUPPORTED_VERSION;
    }
    SECTION("a feature the host requires is missing") {
        offer.features = fm::FeatureFlags::CURSOR_OVERLAY | fm::FeatureFlags::FEC;
        offer.requiredFeatures = fm::FeatureFlags::CURSOR_OVERLAY;
        hello = helloPacket(TABLET_SESSION, 1, 1, 2944, 1840, BOTH_CODECS, fm::FeatureFlags::FEC);
        expected = fm::RejectReason::MISSING_FEATURE;
    }
    HostFixture host(offer);
    host.sessions.handlePacket(hello, TABLET_ADDRESS, START);

    REQUIRE(host.transport.sent.size() == 1);
    CHECK(host.transport.sent[0].type() == fm::PacketType::REJECT);
    CHECK(host.transport.sent[0].to == TABLET_ADDRESS);
    fm::Reject reject;
    reject.read(host.transport.sent[0].bytes);
    CHECK(reject.commonHeader.sessionId == TABLET_SESSION);
    CHECK(reject.controlHeader.msgId == 0);
    CHECK(reject.reason == static_cast<std::uint8_t>(expected));
    CHECK(reject.version == fm::PROTOCOL_VERSION);
    CHECK(host.listener.started.empty());
}

TEST_CASE("a tablet with every required feature gets a session that uses them") {
    fm::HostOffer offer = OFFER;
    offer.features = fm::FeatureFlags::CURSOR_OVERLAY | fm::FeatureFlags::FEC;
    offer.requiredFeatures = fm::FeatureFlags::CURSOR_OVERLAY;
    HostFixture host(offer);
    host.sessions.handlePacket(
            helloPacket(TABLET_SESSION, 100, 1, 2944, 1840, BOTH_CODECS, fm::FeatureFlags::CURSOR_OVERLAY),
            TABLET_ADDRESS, START);
    host.sessions.handlePacket(ackPacket(TABLET_SESSION, 101, 1), TABLET_ADDRESS, START);

    const fm::Session* session = host.sessions.streamingSession();
    REQUIRE(session != nullptr);
    CHECK(session->features == fm::FeatureFlags::CURSOR_OVERLAY);
    CHECK(host.transport.ofType(fm::PacketType::REJECT).empty());
}

TEST_CASE("a SHAPE_REQUEST is passed on once, and only in a streaming session with the cursor overlay") {
    fm::HostOffer offer = OFFER;
    offer.features = fm::FeatureFlags::CURSOR_OVERLAY;
    HostFixture host(offer);
    const auto features = GENERATE(fm::FeatureFlags::CURSOR_OVERLAY, NO_FEATURES);
    host.sessions.handlePacket(helloPacket(TABLET_SESSION, 100, 1, 2944, 1840, BOTH_CODECS, features), TABLET_ADDRESS,
                               START);

    host.sessions.handlePacket(shapeRequestPacket(TABLET_SESSION, 101, 3), TABLET_ADDRESS, START);
    CHECK_FALSE(host.sessions.takeShapeRequest());

    host.sessions.handlePacket(ackPacket(TABLET_SESSION, 102, 1), TABLET_ADDRESS, START);
    host.sessions.handlePacket(shapeRequestPacket(TABLET_SESSION, 103, 3), TABLET_ADDRESS, START + 1ms);
    host.sessions.handlePacket(shapeRequestPacket(TABLET_SESSION, 104, 4), TABLET_ADDRESS, START + 2ms);
    if (features == fm::FeatureFlags::CURSOR_OVERLAY) {
        CHECK(host.sessions.takeShapeRequest() == 4U);
    }
    CHECK_FALSE(host.sessions.takeShapeRequest());
}

TEST_CASE("invalid packets and packets of other sessions change nothing") {
    HostFixture host;
    host.establish();
    std::vector<std::uint8_t> packet;
    SECTION("a HELLO with sessionId 0") {
        packet = helloPacket(0, 1, 1);
    }
    SECTION("a BYE with a reserved bit set") {
        packet = byePacket(TABLET_SESSION, 102);
        packet[12] = 1;
    }
    SECTION("a BYE of another session") {
        packet = byePacket(OTHER_SESSION, 102);
    }
    SECTION("a truncated STATS") {
        packet = statsPacket(TABLET_SESSION, 102, 1, 1);
        packet.pop_back();
    }
    host.sessions.handlePacket(packet, OTHER_TABLET_ADDRESS, START + 10ms);
    CHECK(host.transport.sent.empty());
    CHECK(host.listener.ended.empty());
    REQUIRE(host.sessions.streamingSession() != nullptr);
    CHECK(host.sessions.streamingSession()->peer == TABLET_ADDRESS);
}

TEST_CASE("the host follows the tablet to a new address only for newer packets") {
    HostFixture host;
    host.establish();
    host.sessions.handlePacket(heartbeatPacket(TABLET_SESSION, 110, 5), OTHER_TABLET_ADDRESS, START + 10ms);
    REQUIRE(host.sessions.streamingSession() != nullptr);
    CHECK(host.sessions.streamingSession()->peer == OTHER_TABLET_ADDRESS);
    host.sessions.handlePacket(heartbeatPacket(TABLET_SESSION, 105, 5), TABLET_ADDRESS, START + 20ms);
    CHECK(host.sessions.streamingSession()->peer == OTHER_TABLET_ADDRESS);
}

TEST_CASE("control packets and video share one seq counter per session") {
    HostFixture host;
    host.establish();
    fm::Session* session = host.sessions.streamingSession();
    REQUIRE(session != nullptr);
    const std::uint32_t beforeVideo = session->seq.peek();
    for (int packet = 0; packet < 3; ++packet) {
        static_cast<void>(session->seq.next());
    }
    host.sessions.poll(START + 500ms);
    const auto heartbeats = host.transport.ofType(fm::PacketType::HEARTBEAT);
    REQUIRE(heartbeats.size() == 1);
    CHECK(heartbeats[0].header().seq == beforeVideo + 3);
}

TEST_CASE("close sends three BYEs 20 ms apart and then reports that it is done") {
    HostFixture host;
    host.establish();
    host.sessions.close(START + 1s);
    CHECK(host.sessions.closing());
    host.sessions.poll(START + 1s + 20ms);
    host.sessions.poll(START + 1s + 40ms);
    CHECK(host.transport.ofType(fm::PacketType::BYE).size() == 3);
    CHECK_FALSE(host.sessions.closing());
    CHECK(host.listener.ended == std::vector{std::pair{TABLET_SESSION, fm::SessionEndReason::CLOSED}});
}

TEST_CASE("statsDifference gives the change between two STATS of a session, modulo 2^32") {
    fm::Stats earlier;
    earlier.commonHeader.sessionId = TABLET_SESSION;
    earlier.commonHeader.timestampUs = 1'000'000;
    earlier.packetsReceived = 100;
    earlier.packetsLost = 5;
    earlier.framesCompleted = 10;
    earlier.framesDropped = 1;
    earlier.bytesReceived = 0xFFFF'FF00;
    fm::Stats later = earlier;
    later.commonHeader.timestampUs = 2'000'000;
    later.packetsReceived = 160;
    later.packetsLost = 4;
    later.framesCompleted = 70;
    later.bytesReceived = 0x0000'0100;

    const auto difference = fm::statsDifference(earlier, later);
    REQUIRE(difference);
    CHECK(difference->interval == 1s);
    CHECK(difference->packetsReceived == 60);
    CHECK(difference->packetsLost == -1);
    CHECK(difference->framesCompleted == 60);
    CHECK(difference->framesDropped == 0);
    CHECK(difference->bytesReceived == 0x200);
}

TEST_CASE("statsDifference refuses STATS of different sessions or out of time order") {
    fm::Stats earlier;
    earlier.commonHeader.sessionId = TABLET_SESSION;
    earlier.commonHeader.timestampUs = 1'000'000;
    fm::Stats later = earlier;
    later.commonHeader.timestampUs = 2'000'000;
    CHECK(fm::statsDifference(earlier, later));
    CHECK_FALSE(fm::statsDifference(later, earlier));
    CHECK_FALSE(fm::statsDifference(earlier, earlier));
    later.commonHeader.sessionId = OTHER_SESSION;
    CHECK_FALSE(fm::statsDifference(earlier, later));
}
