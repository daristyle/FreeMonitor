#include "ControlTestHelpers.hpp"
#include "CursorPackets.hpp"
#include "Fragmenter.hpp"
#include "FrameReceiver.hpp"
#include "HostSessions.hpp"
#include "Timestamp.hpp"
#include "UDPControlTransport.hpp"
#include "VideoSender.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::chrono::milliseconds WAIT_TIMEOUT{2000};
    constexpr std::chrono::milliseconds SHORT_RESTART_TIMEOUT{150};
    constexpr std::uint32_t DEVELOPMENT_SESSION_ID = 0;

    std::unique_ptr<fm::FrameReceiver>
    openReceiver(std::chrono::milliseconds senderRestartTimeout = fm::FrameReceiver::DEFAULT_SENDER_RESTART_TIMEOUT) {
        auto receiver = fm::FrameReceiver::open(fm::Endpoint{fm::LOOPBACK_IP_ADDRESS, 0}, DEVELOPMENT_SESSION_ID,
                                                senderRestartTimeout);
        REQUIRE(receiver);
        return std::move(*receiver);
    }

    fm::UDPSocket openSenderSocket() {
        auto udpSocket = fm::UDPSocket::open(fm::Endpoint{fm::LOOPBACK_IP_ADDRESS, 0});
        REQUIRE(udpSocket);
        return std::move(*udpSocket);
    }

    std::vector<std::uint8_t> makeFrameData(std::size_t size, std::uint8_t seed) {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<std::uint8_t>(i * 13 + seed);
        }
        return data;
    }

    fm::EncodedFrame makeEncodedFrame(const std::vector<std::uint8_t>& data, bool keyframe) {
        return fm::EncodedFrame{.data = data, .keyframe = keyframe, .captureTime = std::chrono::steady_clock::now()};
    }

    bool waitUntil(const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + WAIT_TIMEOUT;
        while (!condition()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    struct Sender {
        fm::UDPSocket udpSocket = openSenderSocket();
        fm::VideoSender videoSender;
        fm::Session session;

        Sender(const fm::Endpoint& destination, std::uint32_t firstFrameId, std::uint32_t sessionId = 0)
            : videoSender(udpSocket, firstFrameId),
              session{.peer = destination, .sessionId = sessionId, .seq = fm::SequenceCounter(77)} {}

        void send(const std::vector<std::uint8_t>& data, bool keyframe = true) {
            REQUIRE(videoSender.send(makeEncodedFrame(data, keyframe), fm::Codec::JPEG, session).status ==
                    fm::FrameSendStatus::FRAME_SENT);
        }
    };

    fm::Endpoint endpointOf(fm::FrameReceiver& receiver) {
        const auto endpoint = receiver.localEndpoint();
        REQUIRE(endpoint);
        return *endpoint;
    }

    constexpr fm::HostOffer OFFER{.width = 2944, .height = 1840, .fps = 60, .codecs = BOTH_CODECS, .features = 0};
    constexpr fm::HostOffer FEC_OFFER{
            .width = 2944, .height = 1840, .fps = 60, .codecs = BOTH_CODECS, .features = fm::FeatureFlags::FEC};
    constexpr fm::HostOffer CURSOR_OFFER{.width = 2944,
                                         .height = 1840,
                                         .fps = 60,
                                         .codecs = BOTH_CODECS,
                                         .features = fm::FeatureFlags::CURSOR_OVERLAY,
                                         .requiredFeatures = fm::FeatureFlags::CURSOR_OVERLAY};
    constexpr std::uint32_t FIRST_FRAME_ID = 1000;

    fm::CursorImage cursorImage() {
        fm::CursorImage image{.width = 24,
                              .height = 24,
                              .hotspotX = 5,
                              .hotspotY = 1,
                              .pixels = std::vector<std::uint8_t>(24 * 24 * 4)};
        for (std::size_t i = 0; i < image.pixels.size(); ++i) {
            image.pixels[i] = static_cast<std::uint8_t>(i * 3);
        }
        return image;
    }

    void sendCursor(fm::UDPSocket& udpSocket, const fm::Endpoint& to, std::uint32_t sessionId, fm::SequenceCounter& seq,
                    std::uint32_t shapeId, bool withShape, std::int32_t x) {
        std::array<std::uint8_t, fm::MAX_PACKET_SIZE> packet{};
        if (withShape) {
            const fm::CursorImage image = cursorImage();
            for (std::uint16_t index = 0; index < fm::cursorShapeFragmentCount(image); ++index) {
                const std::size_t size =
                        fm::writeCursorShapeFragment(image, shapeId, index, sessionId, seq.next(), 1, packet);
                REQUIRE(udpSocket.sendTo(std::span(packet).first(size), to) == fm::SendStatus::SENT);
            }
        }
        const fm::CursorState state{.x = x, .y = 200, .visible = true, .imageId = shapeId};
        const std::size_t size = fm::writeCursorPos(state, sessionId, seq.next(), 1, packet);
        REQUIRE(udpSocket.sendTo(std::span(packet).first(size), to) == fm::SendStatus::SENT);
    }

    fm::CursorSnapshot cursorOf(const fm::FrameReceiver& receiver) {
        return fm::CursorSnapshot::unpack(receiver.cursorSnapshot());
    }

    std::unique_ptr<fm::FrameReceiver> openSessionReceiver(const fm::TabletSettings& settings) {
        auto receiver = fm::FrameReceiver::openSession(settings);
        REQUIRE(receiver);
        return std::move(*receiver);
    }

    struct TestHost {
        fm::UDPSocket udpSocket = openSenderSocket();
        fm::UDPControlTransport transport{udpSocket};
        RecordingListener listener;
        fm::HostSessions sessions;
        fm::VideoSender videoSender;
        std::uint32_t byesReceived = 0;
        std::uint32_t keyframeRequestsReceived = 0;

        explicit TestHost(fm::HostOffer offer = OFFER, std::uint32_t firstFrameId = FIRST_FRAME_ID)
            : sessions(transport, listener, offer, counterFrom(9000)), videoSender(udpSocket, firstFrameId) {}

        fm::TabletSettings settings(std::uint8_t codecs = BOTH_CODECS) const {
            const auto endpoint = udpSocket.localEndpoint();
            REQUIRE(endpoint);
            return fm::TabletSettings{
                    .host = *endpoint, .width = 2944, .height = 1840, .refreshHz = 60, .codecs = codecs, .features = 0};
        }

        void pump(std::chrono::milliseconds timeout) {
            std::array<std::uint8_t, fm::MAX_CONTROL_PACKET_SIZE> buffer{};
            fm::Endpoint from;
            auto received = udpSocket.receiveFrom(buffer, from, timeout);
            while (received.status == fm::ReceiveStatus::RECEIVED) {
                const auto type = static_cast<fm::PacketType>(buffer[3]);
                byesReceived += type == fm::PacketType::BYE ? 1 : 0;
                keyframeRequestsReceived += type == fm::PacketType::KEYFRAME_REQUEST ? 1 : 0;
                sessions.handlePacket(std::span(buffer).first(received.size), from, received.arrivalTime);
                received = udpSocket.receiveFrom(buffer, from, 0ms);
            }
            sessions.poll(std::chrono::steady_clock::now());
        }

        bool pumpUntil(const std::function<bool()>& condition) {
            const auto deadline = std::chrono::steady_clock::now() + WAIT_TIMEOUT;
            while (!condition()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                pump(1ms);
            }
            return true;
        }

        void pumpFor(std::chrono::milliseconds duration) {
            const auto end = std::chrono::steady_clock::now() + duration;
            while (std::chrono::steady_clock::now() < end) {
                pump(1ms);
            }
        }

        void connect(fm::FrameReceiver& receiver) {
            REQUIRE(pumpUntil([&] {
                return sessions.streamingSession() != nullptr &&
                       receiver.sessionStatus().state == fm::TabletState::CONNECTED;
            }));
            REQUIRE(sessions.takeKeyframeRequest());
        }

        void sendFrame(const std::vector<std::uint8_t>& data, bool keyframe, fm::Codec codec) {
            fm::Session* session = sessions.streamingSession();
            REQUIRE(session != nullptr);
            REQUIRE(videoSender.send(makeEncodedFrame(data, keyframe), codec, *session).status ==
                    fm::FrameSendStatus::FRAME_SENT);
            if (keyframe) {
                sessions.keyframeSent(std::chrono::steady_clock::now());
            }
        }

        void sendFrameLosingFragment(std::uint32_t frameId, const std::vector<std::uint8_t>& data,
                                     std::uint16_t lostFragment, std::uint16_t parityCount) {
            fm::Session* session = sessions.streamingSession();
            REQUIRE(session != nullptr);
            fm::Fragmenter fragmenter;
            REQUIRE(fragmenter.begin(data, frameId, fm::Codec::H264, false, 1));
            std::array<std::uint8_t, fm::MAX_PACKET_SIZE> packet{};
            for (std::uint16_t index = 0; index < fragmenter.fragmentCount(); ++index) {
                if (index != lostFragment) {
                    const std::size_t size =
                            fragmenter.write(index, session->sessionId, session->seq.next(), 1, packet);
                    REQUIRE(udpSocket.sendTo(std::span(packet).first(size), session->peer) == fm::SendStatus::SENT);
                }
            }
            for (std::uint16_t index = 0; index < parityCount; ++index) {
                const std::size_t size =
                        fragmenter.writeParity(index, parityCount, session->sessionId, session->seq.next(), 1, packet);
                REQUIRE(udpSocket.sendTo(std::span(packet).first(size), session->peer) == fm::SendStatus::SENT);
            }
        }

        void sendFrameWithoutLastFragment(std::uint32_t frameId, const std::vector<std::uint8_t>& data) {
            fm::Session* session = sessions.streamingSession();
            REQUIRE(session != nullptr);
            fm::Fragmenter fragmenter;
            REQUIRE(fragmenter.begin(data, frameId, fm::Codec::H264, false, 1));
            REQUIRE(fragmenter.fragmentCount() > 1);
            std::array<std::uint8_t, fm::MAX_PACKET_SIZE> packet{};
            for (std::uint16_t index = 0; index + 1 < fragmenter.fragmentCount(); ++index) {
                const std::size_t size = fragmenter.write(index, session->sessionId, session->seq.next(), 1, packet);
                REQUIRE(udpSocket.sendTo(std::span(packet).first(size), session->peer) == fm::SendStatus::SENT);
            }
        }
    };

    void receiveFrame(fm::FrameReceiver& receiver, TestHost& host, const std::vector<std::uint8_t>& data) {
        fm::ReassembledFrame frame;
        REQUIRE(host.pumpUntil([&] { return receiver.acquire(frame, 0ms) == fm::MailboxStatus::FRAME_READY; }));
        CHECK(std::ranges::equal(frame.data, data));
        receiver.release();
    }
}

TEST_CASE("a sent frame comes out of the receiver with its bytes and metadata") {
    auto receiver = openReceiver();
    receiver->start();
    Sender sender(endpointOf(*receiver), 5000);
    const auto data = makeFrameData(7 * fm::MAX_FRAGMENT_PAYLOAD + 99, 3);
    const auto encodedFrame = makeEncodedFrame(data, true);
    const auto beforeSend = std::chrono::steady_clock::now();
    REQUIRE(sender.videoSender.send(encodedFrame, fm::Codec::JPEG, sender.session).status ==
            fm::FrameSendStatus::FRAME_SENT);

    fm::ReassembledFrame frame;
    REQUIRE(receiver->acquire(frame, WAIT_TIMEOUT) == fm::MailboxStatus::FRAME_READY);
    const auto afterAcquire = std::chrono::steady_clock::now();
    CHECK(frame.firstFragmentTime >= beforeSend);
    CHECK(frame.firstFragmentTime <= frame.completeTime);
    CHECK(frame.completeTime <= afterAcquire);
    CHECK(std::ranges::equal(frame.data, data));
    CHECK(frame.frameId == 5000);
    CHECK(frame.codec == fm::Codec::JPEG);
    CHECK(frame.keyframe);
    CHECK(frame.captureTimestampUs == fm::toTimestampUs(encodedFrame.captureTime));
    receiver->release();

    const auto statistics = receiver->statistics();
    CHECK(statistics.packets == 8);
    CHECK(statistics.framesCompleted == 1);
    CHECK(statistics.invalidPackets == 0);
    CHECK(statistics.ignoredPackets == 0);
}

TEST_CASE("only the newest frame is handed out and older ones count as replaced") {
    auto receiver = openReceiver();
    receiver->start();
    Sender sender(endpointOf(*receiver), 1);
    for (std::uint8_t seed = 1; seed <= 3; ++seed) {
        sender.send(makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, seed));
    }
    REQUIRE(waitUntil([&] { return receiver->statistics().framesCompleted == 3; }));

    fm::ReassembledFrame frame;
    REQUIRE(receiver->acquire(frame, 0ms) == fm::MailboxStatus::FRAME_READY);
    CHECK(frame.frameId == 3);
    CHECK(std::ranges::equal(frame.data, makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, 3)));
    receiver->release();
    CHECK(receiver->statistics().framesReplaced == 2);
    CHECK(receiver->acquire(frame, 0ms) == fm::MailboxStatus::NO_NEW_FRAME);
}

TEST_CASE("acquire times out when nothing is sent") {
    auto receiver = openReceiver();
    receiver->start();
    fm::ReassembledFrame frame;
    CHECK(receiver->acquire(frame, 50ms) == fm::MailboxStatus::NO_NEW_FRAME);
}

TEST_CASE("packets for another session, other packet types and garbage are dropped without effect") {
    auto receiver = openReceiver();
    receiver->start();
    const fm::Endpoint destination = endpointOf(*receiver);

    Sender otherSession(destination, 900, 7);
    otherSession.send(makeFrameData(100, 1));

    auto rawSocket = openSenderSocket();
    const std::array<std::uint8_t, 5> garbage{1, 2, 3, 4, 5};
    REQUIRE(rawSocket.sendTo(garbage, destination) == fm::SendStatus::SENT);

    fm::KeyframeRequest keyframeRequest;
    keyframeRequest.commonHeader = fm::makeCommonHeader(fm::PacketType::KEYFRAME_REQUEST);
    std::array<std::uint8_t, fm::KeyframeRequest::SIZE> requestBytes{};
    REQUIRE(keyframeRequest.write(requestBytes) == fm::KeyframeRequest::SIZE);
    REQUIRE(rawSocket.sendTo(requestBytes, destination) == fm::SendStatus::SENT);

    std::array<std::uint8_t, fm::KeyframeRequest::SIZE> wrongMagic = requestBytes;
    wrongMagic[0] = 0;
    REQUIRE(rawSocket.sendTo(wrongMagic, destination) == fm::SendStatus::SENT);

    REQUIRE(waitUntil([&] { return receiver->statistics().packets == 4; }));
    const auto statistics = receiver->statistics();
    CHECK(statistics.ignoredPackets == 2);
    CHECK(statistics.invalidPackets == 2);
    fm::ReassembledFrame frame;
    CHECK(receiver->acquire(frame, 0ms) == fm::MailboxStatus::NO_NEW_FRAME);

    Sender developmentSender(destination, 900);
    const auto data = makeFrameData(100, 2);
    developmentSender.send(data);
    REQUIRE(receiver->acquire(frame, WAIT_TIMEOUT) == fm::MailboxStatus::FRAME_READY);
    CHECK(frame.frameId == 900);
    CHECK(std::ranges::equal(frame.data, data));
    receiver->release();
}

TEST_CASE("a restarted sender with older frame IDs is accepted after the restart timeout") {
    auto receiver = openReceiver(SHORT_RESTART_TIMEOUT);
    receiver->start();
    const fm::Endpoint destination = endpointOf(*receiver);
    fm::ReassembledFrame frame;

    Sender firstRun(destination, 100000);
    firstRun.send(makeFrameData(50, 1));
    REQUIRE(receiver->acquire(frame, WAIT_TIMEOUT) == fm::MailboxStatus::FRAME_READY);
    receiver->release();

    Sender restarted(destination, 10);
    restarted.send(makeFrameData(50, 2));
    CHECK(receiver->acquire(frame, 50ms) == fm::MailboxStatus::NO_NEW_FRAME);

    std::this_thread::sleep_for(SHORT_RESTART_TIMEOUT + 50ms);
    const auto data = makeFrameData(50, 3);
    restarted.send(data);
    REQUIRE(receiver->acquire(frame, WAIT_TIMEOUT) == fm::MailboxStatus::FRAME_READY);
    CHECK(frame.frameId == 11);
    CHECK(std::ranges::equal(frame.data, data));
    receiver->release();
    CHECK(receiver->statistics().senderRestarts == 1);
}

TEST_CASE("stop ends the receive thread quickly and wakes a waiting consumer") {
    auto receiver = openReceiver();
    receiver->start();

    fm::MailboxStatus consumerStatus = fm::MailboxStatus::FRAME_READY;
    std::thread consumer([&] {
        fm::ReassembledFrame frame;
        consumerStatus = receiver->acquire(frame, 10s);
    });
    std::this_thread::sleep_for(20ms);

    const auto started = std::chrono::steady_clock::now();
    receiver->stop();
    consumer.join();
    CHECK(std::chrono::steady_clock::now() - started < 1s);
    CHECK(consumerStatus == fm::MailboxStatus::CLOSED);
}

TEST_CASE("a receiver cannot open a port that is already taken") {
    auto receiver = openReceiver();
    const auto second = fm::FrameReceiver::open(endpointOf(*receiver), DEVELOPMENT_SESSION_ID);
    REQUIRE_FALSE(second);
    CHECK_FALSE(second.error().message.empty());
}

TEST_CASE("a session receiver connects to the host and hands out the host's video") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);

    const auto status = receiver->sessionStatus();
    CHECK(status.sessionId == host.sessions.streamingSession()->sessionId);
    CHECK(status.sessionId != 0);
    CHECK(status.stream.codec == fm::Codec::H264);
    CHECK(status.stream.width == OFFER.width);
    CHECK(status.stream.height == OFFER.height);
    CHECK(status.stream.fps == OFFER.fps);
    CHECK(status.sessionCount == 1);

    const auto data = makeFrameData(3 * fm::MAX_FRAGMENT_PAYLOAD + 5, 9);
    host.sendFrame(data, true, fm::Codec::H264);
    fm::ReassembledFrame frame;
    REQUIRE(host.pumpUntil([&] { return receiver->acquire(frame, 0ms) == fm::MailboxStatus::FRAME_READY; }));
    CHECK(std::ranges::equal(frame.data, data));
    CHECK(frame.frameId == FIRST_FRAME_ID);
    CHECK(frame.keyframe);
    CHECK(frame.codec == fm::Codec::H264);
    receiver->release();

    receiver->reconnect();
    host.pumpFor(50ms);
    CHECK(receiver->sessionStatus().sessionCount == 1);
    CHECK(receiver->sessionStatus().state == fm::TabletState::CONNECTED);
}

TEST_CASE("a keyframe request from another thread reaches the host") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);
    const auto keyframe = makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, 1);
    host.sendFrame(keyframe, true, fm::Codec::H264);
    receiveFrame(*receiver, host, keyframe);
    host.pumpFor(150ms);
    REQUIRE(host.keyframeRequestsReceived == 0);

    std::thread([&] { receiver->requestKeyframe(); }).join();
    REQUIRE(host.pumpUntil([&] { return host.sessions.takeKeyframeRequest(); }));
    CHECK(host.keyframeRequestsReceived >= 1);
}

TEST_CASE("an incomplete H.264 frame makes the session receiver ask for a keyframe by itself") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);
    const auto keyframe = makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, 1);
    host.sendFrame(keyframe, true, fm::Codec::H264);
    receiveFrame(*receiver, host, keyframe);
    REQUIRE(host.keyframeRequestsReceived == 0);

    host.sendFrameWithoutLastFragment(FIRST_FRAME_ID + 5, makeFrameData(3 * fm::MAX_FRAGMENT_PAYLOAD, 2));
    REQUIRE(host.pumpUntil([&] { return host.sessions.takeKeyframeRequest(); }));
    CHECK(receiver->statistics().framesDropped == 1);
}

TEST_CASE("stop sends three BYEs, and the host ends the session") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);
    const std::uint32_t sessionId = receiver->sessionStatus().sessionId;

    const auto started = std::chrono::steady_clock::now();
    receiver->stop();
    CHECK(std::chrono::steady_clock::now() - started < 1s);
    REQUIRE(host.pumpUntil([&] { return host.byesReceived == 3; }));
    CHECK(host.listener.ended == std::vector{std::pair{sessionId, fm::SessionEndReason::BYE}});
    CHECK(host.sessions.streamingSession() == nullptr);
    CHECK(receiver->sessionStatus().state == fm::TabletState::IDLE);
}

TEST_CASE("a rejected session receiver keeps the reason and connects again only on reconnect") {
    TestHost host(fm::HostOffer{.width = 2944, .height = 1840, .fps = 60, .codecs = fm::CodecFlags::H264});
    auto receiver = openSessionReceiver(host.settings(fm::CodecFlags::JPEG));
    receiver->start();
    REQUIRE(host.pumpUntil([&] { return receiver->sessionStatus().state == fm::TabletState::REJECTED; }));
    CHECK(receiver->sessionStatus().rejectReason == static_cast<std::uint8_t>(fm::RejectReason::NO_COMMON_CODEC));
    host.pumpFor(600ms);
    CHECK(host.listener.rejected.size() == 1);

    host.sessions.setOffer(OFFER);
    receiver->reconnect();
    host.connect(*receiver);
    const auto status = receiver->sessionStatus();
    CHECK(status.sessionCount == 2);
    CHECK(status.stream.codec == fm::Codec::JPEG);
}

TEST_CASE("after the host ends the session, a reconnect starts a new one that accepts lower frame IDs at once") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);
    const auto first = makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, 1);
    host.sendFrame(first, true, fm::Codec::H264);
    receiveFrame(*receiver, host, first);
    const std::uint32_t firstSessionId = receiver->sessionStatus().sessionId;

    host.sessions.close(std::chrono::steady_clock::now());
    REQUIRE(host.pumpUntil([&] { return receiver->sessionStatus().state == fm::TabletState::ENDED_BY_HOST; }));
    host.pumpFor(600ms);
    CHECK(receiver->sessionStatus().state == fm::TabletState::ENDED_BY_HOST);

    receiver->reconnect();
    host.connect(*receiver);
    CHECK(receiver->sessionStatus().sessionId != firstSessionId);
    CHECK(receiver->sessionStatus().sessionCount == 2);
    fm::VideoSender restartedSender(host.udpSocket, 10);
    const auto second = makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, 2);
    REQUIRE(restartedSender.send(makeEncodedFrame(second, true), fm::Codec::H264, *host.sessions.streamingSession())
                    .status == fm::FrameSendStatus::FRAME_SENT);
    receiveFrame(*receiver, host, second);
    CHECK(receiver->statistics().senderRestarts == 0);
}

TEST_CASE("a session receiver ignores video of another session") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);
    fm::Session otherSession = *host.sessions.streamingSession();
    otherSession.sessionId += 1;
    REQUIRE(host.videoSender
                    .send(makeEncodedFrame(makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, 7), true), fm::Codec::H264,
                          otherSession)
                    .status == fm::FrameSendStatus::FRAME_SENT);
    const auto data = makeFrameData(2 * fm::MAX_FRAGMENT_PAYLOAD, 8);
    host.sendFrame(data, true, fm::Codec::H264);
    receiveFrame(*receiver, host, data);
    CHECK(receiver->statistics().ignoredPackets >= 2);
    CHECK(receiver->statistics().framesCompleted == 1);
}

TEST_CASE("a session receiver with FEC rebuilds a frame that lost a fragment") {
    TestHost host(FEC_OFFER);
    fm::TabletSettings settings = host.settings();
    settings.features = fm::FeatureFlags::FEC;
    auto receiver = openSessionReceiver(settings);
    receiver->start();
    host.connect(*receiver);
    CHECK(receiver->sessionStatus().stream.features == fm::FeatureFlags::FEC);

    const auto data = makeFrameData(3 * fm::MAX_FRAGMENT_PAYLOAD + 5, 9);
    host.sendFrameLosingFragment(FIRST_FRAME_ID, data, 1, 2);
    receiveFrame(*receiver, host, data);

    const auto statistics = receiver->statistics();
    CHECK(statistics.framesCompleted == 1);
    CHECK(statistics.framesRepaired == 1);
    CHECK(statistics.framesDropped == 0);
    CHECK(statistics.invalidPackets == 0);
}

TEST_CASE("the parity of a whole frame arrives late and does no harm") {
    TestHost host(FEC_OFFER);
    fm::TabletSettings settings = host.settings();
    settings.features = fm::FeatureFlags::FEC;
    auto receiver = openSessionReceiver(settings);
    receiver->start();
    host.connect(*receiver);

    const auto data = makeFrameData(3 * fm::MAX_FRAGMENT_PAYLOAD + 5, 9);
    host.sendFrame(data, true, fm::Codec::H264);
    REQUIRE(host.videoSender.sendParity(30, *host.sessions.streamingSession()).packetsSent == 2);
    receiveFrame(*receiver, host, data);
    host.pumpFor(50ms);

    const auto statistics = receiver->statistics();
    CHECK(statistics.framesCompleted == 1);
    CHECK(statistics.framesRepaired == 0);
    CHECK(statistics.framesDropped == 0);
    CHECK(statistics.invalidPackets == 0);
    CHECK(statistics.senderRestarts == 0);
}

TEST_CASE("a session without FEC ignores parity") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);

    const auto data = makeFrameData(3 * fm::MAX_FRAGMENT_PAYLOAD + 5, 9);
    host.sendFrameLosingFragment(FIRST_FRAME_ID, data, 1, 2);
    REQUIRE(host.pumpUntil([&] { return receiver->statistics().ignoredPackets == 2; }));

    const auto statistics = receiver->statistics();
    CHECK(statistics.framesCompleted == 0);
    CHECK(statistics.framesRepaired == 0);
}

TEST_CASE("parity in development mode is ignored") {
    auto receiver = openReceiver();
    receiver->start();
    Sender sender(endpointOf(*receiver), 5);
    const auto data = makeFrameData(fm::MAX_FRAGMENT_PAYLOAD, 3);
    fm::Fragmenter fragmenter;
    REQUIRE(fragmenter.begin(data, 5, fm::Codec::JPEG, true, 1));
    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> packet{};
    const std::size_t size = fragmenter.writeParity(0, 2, 0, 1, 1, packet);
    REQUIRE(size != 0);
    REQUIRE(sender.udpSocket.sendTo(std::span(packet).first(size), endpointOf(*receiver)) == fm::SendStatus::SENT);

    REQUIRE(waitUntil([&] { return receiver->statistics().ignoredPackets == 1; }));
    CHECK(receiver->statistics().framesCompleted == 0);
}

TEST_CASE("development mode draws the cursor from cursor packets of session 0") {
    auto receiver = openReceiver();
    receiver->start();
    Sender sender(endpointOf(*receiver), 900);
    sendCursor(sender.udpSocket, endpointOf(*receiver), DEVELOPMENT_SESSION_ID, sender.session.seq, 1, true, 100);
    REQUIRE(waitUntil([&] { return cursorOf(*receiver).visible; }));
    CHECK(cursorOf(*receiver).x == 100);
    CHECK(cursorOf(*receiver).y == 200);
    std::vector<std::uint32_t> pixels(fm::CursorShapeCache::MAX_PIXELS);
    CHECK(receiver->copyCursorShape(receiver->cursorSnapshot(), pixels) ==
          fm::CursorShapeInfo{.width = 24, .height = 24, .hotspotX = 5, .hotspotY = 1});

    sendCursor(sender.udpSocket, endpointOf(*receiver), 7, sender.session.seq, 1, false, 300);
    REQUIRE(waitUntil([&] { return receiver->statistics().ignoredPackets == 1; }));
    CHECK(cursorOf(*receiver).x == 100);
}

TEST_CASE("a session with the cursor overlay draws the host's cursor and asks again for a lost shape") {
    TestHost host(CURSOR_OFFER);
    fm::TabletSettings settings = host.settings();
    settings.features = fm::FeatureFlags::CURSOR_OVERLAY;
    auto receiver = openSessionReceiver(settings);
    receiver->start();
    host.connect(*receiver);
    fm::Session* session = host.sessions.streamingSession();
    REQUIRE(session != nullptr);

    sendCursor(host.udpSocket, session->peer, session->sessionId, session->seq, 1, true, 100);
    REQUIRE(host.pumpUntil([&] { return cursorOf(*receiver).visible; }));

    sendCursor(host.udpSocket, session->peer, session->sessionId, session->seq, 2, false, 101);
    REQUIRE(host.pumpUntil([&] { return host.sessions.takeShapeRequest() == 2U; }));
    CHECK(cursorOf(*receiver).x == 101);
    CHECK(cursorOf(*receiver).visible);

    host.sessions.close(std::chrono::steady_clock::now());
    REQUIRE(host.pumpUntil([&] { return !cursorOf(*receiver).visible; }));
    CHECK(cursorOf(*receiver) == fm::CursorSnapshot{});
}

TEST_CASE("a session without the cursor overlay ignores cursor packets") {
    TestHost host;
    auto receiver = openSessionReceiver(host.settings());
    receiver->start();
    host.connect(*receiver);
    fm::Session* session = host.sessions.streamingSession();
    REQUIRE(session != nullptr);

    sendCursor(host.udpSocket, session->peer, session->sessionId, session->seq, 1, true, 100);
    REQUIRE(host.pumpUntil([&] { return receiver->statistics().ignoredPackets == 3; }));
    CHECK(cursorOf(*receiver) == fm::CursorSnapshot{});
}
