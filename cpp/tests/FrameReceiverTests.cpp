#include "FrameReceiver.hpp"
#include "Timestamp.hpp"
#include "VideoSender.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
              session{.peer = destination, .sessionId = sessionId, .nextSeq = 77} {}

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
