#include "ControlTestHelpers.hpp"
#include "TabletSession.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <vector>

namespace {
    constexpr fm::TabletSettings SETTINGS{
            .host = HOST_ADDRESS, .width = 2944, .height = 1840, .refreshHz = 60, .codecs = BOTH_CODECS, .features = 0};

    constexpr fm::TabletSettings JPEG_ONLY_SETTINGS{.host = HOST_ADDRESS,
                                                    .width = 2944,
                                                    .height = 1840,
                                                    .refreshHz = 60,
                                                    .codecs = fm::CodecFlags::JPEG,
                                                    .features = 0};

    constexpr std::uint8_t UNASSIGNED_FEATURE = 0x80;

    struct UnusableWelcome {
        fm::TabletSettings settings;
        fm::Codec codec;
        std::uint16_t width;
        std::uint8_t features;
    };

    struct TabletFixture {
        FakeControlTransport transport;
        fm::TabletSession session;

        explicit TabletFixture(fm::TabletSettings settings = SETTINGS)
            : session(transport, settings, counterFrom(500)) {}

        std::uint32_t id() const {
            return session.sessionId();
        }

        void connect(std::chrono::steady_clock::time_point now = START) {
            session.start(now);
            session.handlePacket(ackPacket(id(), 9000, 1), HOST_ADDRESS, now);
            session.handlePacket(welcomePacket(id(), 9001, 1), HOST_ADDRESS, now);
            transport.sent.clear();
        }

        std::vector<std::uint8_t> videoPacket(std::uint32_t seq) const {
            fm::VideoFragment fragment;
            fragment.commonHeader.sessionId = id();
            fragment.commonHeader.seq = seq;
            std::vector<std::uint8_t> bytes(fm::VideoFragment::SIZE);
            fragment.write(bytes);
            return bytes;
        }
    };
}

TEST_CASE("start sends a HELLO with the screen and codecs, repeated every 500 ms until acknowledged") {
    TabletFixture tablet;
    tablet.session.start(START);
    REQUIRE(tablet.transport.sent.size() == 1);
    const SentPacket first = tablet.transport.sent[0];
    CHECK(first.type() == fm::PacketType::HELLO);
    CHECK(first.to == HOST_ADDRESS);
    fm::Hello hello;
    hello.read(first.bytes);
    CHECK(hello.commonHeader.sessionId != 0);
    CHECK(hello.controlHeader.msgId == 1);
    CHECK(hello.width == 2944);
    CHECK(hello.height == 1840);
    CHECK(hello.refreshHz == 60);
    CHECK(hello.codecs == BOTH_CODECS);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTING);

    tablet.session.poll(START + 499ms);
    CHECK(tablet.transport.sent.size() == 1);
    tablet.session.poll(START + 500ms);
    REQUIRE(tablet.transport.sent.size() == 2);
    CHECK(tablet.transport.sent[1].controlHeader().msgId == 1);
    CHECK(tablet.transport.sent[1].header().seq == first.header().seq + 1);

    tablet.session.handlePacket(ackPacket(tablet.id(), 9000, 1), HOST_ADDRESS, START + 600ms);
    tablet.session.poll(START + 1000ms);
    CHECK(tablet.transport.ofType(fm::PacketType::HELLO).size() == 2);
}

TEST_CASE("a WELCOME connects the tablet, and every copy of it is acknowledged") {
    TabletFixture tablet;
    tablet.session.start(START);
    tablet.transport.sent.clear();
    tablet.session.handlePacket(welcomePacket(tablet.id(), 9000, 1, fm::Codec::H264, 2944, 1840), HOST_ADDRESS,
                                START + 10ms);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTED);
    CHECK(tablet.session.stream().codec == fm::Codec::H264);
    CHECK(tablet.session.stream().width == 2944);
    CHECK(tablet.session.stream().fps == 60);
    tablet.session.handlePacket(welcomePacket(tablet.id(), 9001, 1), HOST_ADDRESS, START + 20ms);

    const auto acks = tablet.transport.ofType(fm::PacketType::ACK);
    REQUIRE(acks.size() == 2);
    fm::ACK ack;
    ack.read(acks[1].bytes);
    CHECK(ack.ackedMsgId == 1);
}

TEST_CASE("a WELCOME also stops the HELLO resends when the ACK for the HELLO was lost") {
    TabletFixture tablet;
    tablet.session.start(START);
    tablet.session.handlePacket(welcomePacket(tablet.id(), 9000, 1), HOST_ADDRESS, START + 10ms);
    tablet.transport.sent.clear();
    tablet.session.poll(START + 500ms);
    tablet.session.poll(START + 1000ms);
    CHECK(tablet.transport.ofType(fm::PacketType::HELLO).empty());
}

TEST_CASE("a WELCOME the tablet cannot use is dropped without an ACK") {
    const auto unusable = GENERATE(UnusableWelcome{JPEG_ONLY_SETTINGS, fm::Codec::H264, 2944, 0},
                                   UnusableWelcome{SETTINGS, fm::Codec::H264, 2943, 0},
                                   UnusableWelcome{SETTINGS, static_cast<fm::Codec>(5), 2944, 0},
                                   UnusableWelcome{SETTINGS, static_cast<fm::Codec>(200), 2944, 0},
                                   UnusableWelcome{SETTINGS, fm::Codec::H264, 2944, fm::FeatureFlags::CURSOR_OVERLAY});
    TabletFixture tablet(unusable.settings);
    tablet.session.start(START);
    tablet.transport.sent.clear();
    tablet.session.handlePacket(
            welcomePacket(tablet.id(), 9000, 1, unusable.codec, unusable.width, 1840, unusable.features), HOST_ADDRESS,
            START + 10ms);
    CHECK(tablet.transport.sent.empty());
    CHECK(tablet.session.state() == fm::TabletState::CONNECTING);
}

TEST_CASE("the tablet keeps FEC when it asked for it and the host grants it") {
    fm::TabletSettings settings = SETTINGS;
    settings.features = fm::FeatureFlags::FEC;
    TabletFixture tablet(settings);
    tablet.session.start(START);
    tablet.session.handlePacket(welcomePacket(tablet.id(), 9000, 1, fm::Codec::H264, 2944, 1840, fm::FeatureFlags::FEC),
                                HOST_ADDRESS, START + 10ms);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTED);
    CHECK(tablet.session.stream().features == fm::FeatureFlags::FEC);
}

TEST_CASE("feature bits the tablet does not know are ignored and do not stop the WELCOME") {
    fm::TabletSettings settings = SETTINGS;
    settings.features = fm::FeatureFlags::CURSOR_OVERLAY;
    TabletFixture tablet(settings);
    tablet.session.start(START);
    tablet.transport.sent.clear();
    tablet.session.handlePacket(welcomePacket(tablet.id(), 9000, 1, fm::Codec::H264, 2944, 1840,
                                              fm::FeatureFlags::CURSOR_OVERLAY | UNASSIGNED_FEATURE),
                                HOST_ADDRESS, START + 10ms);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTED);
    CHECK(tablet.session.stream().features == fm::FeatureFlags::CURSOR_OVERLAY);
    CHECK(tablet.transport.ofType(fm::PacketType::ACK).size() == 1);
}

TEST_CASE("a REJECT stops the HELLOs and keeps the reason, whatever its protocol version") {
    TabletFixture tablet;
    tablet.session.start(START);
    tablet.session.handlePacket(rejectPacket(tablet.id(), 2, 9), HOST_ADDRESS, START + 10ms);
    CHECK(tablet.session.state() == fm::TabletState::REJECTED);
    CHECK(tablet.session.rejectReason() == 2);
    tablet.transport.sent.clear();
    tablet.session.poll(START + 10s);
    CHECK(tablet.transport.sent.empty());
}

TEST_CASE("a BYE ends the session, and the tablet does not reconnect on its own") {
    TabletFixture tablet;
    tablet.connect();
    tablet.session.handlePacket(byePacket(tablet.id(), 9002), HOST_ADDRESS, START + 1s);
    CHECK(tablet.session.state() == fm::TabletState::ENDED_BY_HOST);
    tablet.session.poll(START + 10s);
    CHECK(tablet.transport.sent.empty());
    CHECK(tablet.session.sessionCount() == 1);
}

TEST_CASE("after 3 s without a packet from the host, the tablet starts a new session with a new sessionId") {
    TabletFixture tablet;
    tablet.connect();
    const std::uint32_t firstId = tablet.id();
    tablet.session.poll(START + 2999ms);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTED);
    tablet.transport.sent.clear();
    tablet.session.poll(START + 3s);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTING);
    CHECK(tablet.id() != firstId);
    CHECK(tablet.session.sessionCount() == 2);
    const auto hellos = tablet.transport.ofType(fm::PacketType::HELLO);
    REQUIRE(hellos.size() == 1);
    CHECK(hellos[0].header().sessionId == tablet.id());
}

TEST_CASE("before the host answers at all, the tablet keeps sending the same HELLO") {
    TabletFixture tablet;
    tablet.session.start(START);
    const std::uint32_t id = tablet.id();
    tablet.session.poll(START + 10s);
    CHECK(tablet.id() == id);
    CHECK(tablet.session.sessionCount() == 1);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTING);
}

TEST_CASE("a connected tablet sends heartbeats every 500 ms and STATS with its counters every second") {
    TabletFixture tablet;
    tablet.connect();
    tablet.session.frameCompleted();
    tablet.session.frameCompleted();
    tablet.session.frameDropped();
    tablet.session.handlePacket(heartbeatPacket(tablet.id(), 9005, 5), HOST_ADDRESS, START + 100ms);
    tablet.session.poll(START);
    tablet.session.poll(START + 500ms);
    tablet.session.poll(START + 1s);
    CHECK(tablet.transport.ofType(fm::PacketType::HEARTBEAT).size() == 3);
    const auto stats = tablet.transport.ofType(fm::PacketType::STATS);
    REQUIRE(stats.size() == 1);
    fm::Stats counters;
    counters.read(stats[0].bytes);
    CHECK(counters.packetsReceived == 3);
    CHECK(counters.packetsLost == 3);
    CHECK(counters.framesCompleted == 2);
    CHECK(counters.framesDropped == 1);
    CHECK(counters.bytesReceived == fm::ACK::SIZE + fm::Welcome::SIZE + fm::Heartbeat::SIZE);
}

TEST_CASE("a keyframe request is repeated every 200 ms until a keyframe arrives, and not more often") {
    TabletFixture tablet;
    tablet.connect();
    tablet.session.handlePacket(heartbeatPacket(tablet.id(), 9002, 5), HOST_ADDRESS, START);
    tablet.session.requestKeyframe(START + 10ms);
    tablet.session.requestKeyframe(START + 50ms);
    CHECK(tablet.transport.ofType(fm::PacketType::KEYFRAME_REQUEST).size() == 1);
    tablet.session.poll(START + 209ms);
    CHECK(tablet.transport.ofType(fm::PacketType::KEYFRAME_REQUEST).size() == 1);
    tablet.session.poll(START + 210ms);
    CHECK(tablet.transport.ofType(fm::PacketType::KEYFRAME_REQUEST).size() == 2);
    tablet.session.keyframeReceived();
    tablet.session.poll(START + 410ms);
    CHECK(tablet.transport.ofType(fm::PacketType::KEYFRAME_REQUEST).size() == 2);
}

TEST_CASE("media packets of the session are handed on only while connected") {
    TabletFixture tablet;
    tablet.session.start(START);
    CHECK(tablet.session.handlePacket(tablet.videoPacket(9000), HOST_ADDRESS, START) == fm::SessionPacket::IGNORED);
    tablet.session.handlePacket(welcomePacket(tablet.id(), 9001, 1), HOST_ADDRESS, START);
    CHECK(tablet.session.handlePacket(tablet.videoPacket(9002), HOST_ADDRESS, START) == fm::SessionPacket::MEDIA);
    auto foreign = tablet.videoPacket(9003);
    foreign[7] ^= 0x01;
    CHECK(tablet.session.handlePacket(foreign, HOST_ADDRESS, START) == fm::SessionPacket::IGNORED);
    CHECK(tablet.session.handlePacket(ackPacket(tablet.id(), 9004, 1), HOST_ADDRESS, START) ==
          fm::SessionPacket::CONTROL);
}

TEST_CASE("video packets keep the session alive") {
    TabletFixture tablet;
    tablet.connect();
    tablet.session.handlePacket(tablet.videoPacket(9002), HOST_ADDRESS, START + 2s);
    tablet.session.poll(START + 4s);
    CHECK(tablet.session.state() == fm::TabletState::CONNECTED);
    CHECK(tablet.session.sessionCount() == 1);
}

TEST_CASE("closing the tablet sends three BYEs to the host 20 ms apart and then reports that it is done") {
    TabletFixture tablet;
    tablet.connect();
    CHECK_FALSE(tablet.session.closing());
    tablet.session.close(START + 1s);
    CHECK(tablet.session.state() == fm::TabletState::IDLE);
    CHECK(tablet.session.closing());
    tablet.session.poll(START + 1s + 20ms);
    CHECK(tablet.session.closing());
    tablet.session.poll(START + 1s + 40ms);
    CHECK_FALSE(tablet.session.closing());
    tablet.session.poll(START + 1s + 60ms);
    const auto byes = tablet.transport.ofType(fm::PacketType::BYE);
    REQUIRE(byes.size() == 3);
    CHECK(byes[0].to == HOST_ADDRESS);
}

TEST_CASE("closing a tablet that has no session sends nothing and is not closing") {
    TabletFixture tablet;
    tablet.session.close(START);
    CHECK_FALSE(tablet.session.closing());
    CHECK(tablet.transport.sent.empty());
}
