#include "Reassembler.hpp"
#include "Timestamp.hpp"
#include "VideoSender.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::chrono::milliseconds RECEIVE_TIMEOUT{1000};
    constexpr std::chrono::milliseconds NO_MORE_TIMEOUT{50};
    constexpr int RECEIVE_BUFFER_SIZE = 1 << 20;
    constexpr std::uint32_t SESSION_ID = 0x5E55'1011;

    fm::UDPSocket openLoopbackSocket() {
        auto udpSocket = fm::UDPSocket::open(fm::Endpoint{fm::LOOPBACK_IP_ADDRESS, 0});
        REQUIRE(udpSocket);
        return std::move(*udpSocket);
    }

    fm::Endpoint localEndpointOf(const fm::UDPSocket& udpSocket) {
        const auto endpoint = udpSocket.localEndpoint();
        REQUIRE(endpoint);
        return *endpoint;
    }

    std::vector<std::uint8_t> makeFrameData(std::size_t size) {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<std::uint8_t>(i * 7 + i / 251);
        }
        return data;
    }

    fm::EncodedFrame makeEncodedFrame(const std::vector<std::uint8_t>& data, bool keyframe) {
        return fm::EncodedFrame{.data = data, .keyframe = keyframe, .captureTime = std::chrono::steady_clock::now()};
    }

    struct Loopback {
        fm::UDPSocket sender = openLoopbackSocket();
        fm::UDPSocket receiver = openLoopbackSocket();
        fm::Session session;

        explicit Loopback(std::uint32_t firstSeq) {
            (void) receiver.setReceiveBufferSize(RECEIVE_BUFFER_SIZE);
            session = fm::Session{.peer = localEndpointOf(receiver), .sessionId = SESSION_ID, .nextSeq = firstSeq};
        }

        std::vector<std::vector<std::uint8_t>> receive(std::size_t count) {
            std::vector<std::vector<std::uint8_t>> packets;
            std::vector<std::uint8_t> buffer(fm::MAX_PACKET_SIZE);
            for (std::size_t i = 0; i < count; ++i) {
                fm::Endpoint from;
                const auto result = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
                REQUIRE(result.status == fm::ReceiveStatus::RECEIVED);
                CHECK(from == localEndpointOf(sender));
                packets.emplace_back(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(result.size));
            }
            return packets;
        }

        bool nothingMoreArrives() {
            std::vector<std::uint8_t> buffer(fm::MAX_PACKET_SIZE);
            fm::Endpoint from;
            return receiver.receiveFrom(buffer, from, NO_MORE_TIMEOUT).status == fm::ReceiveStatus::NO_DATAGRAM;
        }
    };

    fm::VideoFragment parse(const std::vector<std::uint8_t>& packet) {
        fm::VideoFragment fragment;
        REQUIRE(fragment.read(packet));
        return fragment;
    }
}

TEST_CASE("a sent frame arrives over loopback and reassembles to the same bytes") {
    constexpr std::uint32_t firstSeq = 100;
    constexpr std::uint32_t firstFrameId = 7000;
    constexpr std::size_t frameSize = 7 * fm::MAX_FRAGMENT_PAYLOAD + 123;
    constexpr std::uint16_t expectedPackets = 8;

    Loopback loopback(firstSeq);
    fm::VideoSender videoSender(loopback.sender, firstFrameId);
    const auto data = makeFrameData(frameSize);
    const auto frame = makeEncodedFrame(data, true);

    const auto result = videoSender.send(frame, fm::Codec::JPEG, loopback.session);
    CHECK(result.status == fm::FrameSendStatus::FRAME_SENT);
    CHECK(result.packetsSent == expectedPackets);
    CHECK(result.bytesSent == expectedPackets * fm::VideoFragment::SIZE + frameSize);
    CHECK(loopback.session.nextSeq == firstSeq + expectedPackets);

    const auto packets = loopback.receive(expectedPackets);
    CHECK(loopback.nothingMoreArrives());

    fm::Reassembler reassembler;
    std::uint64_t previousTimestampUs = fm::toTimestampUs(frame.captureTime);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        CAPTURE(i);
        const auto fragment = parse(packets[i]);
        CHECK(fragment.commonHeader.magic == fm::MAGIC);
        CHECK(fragment.commonHeader.version == fm::PROTOCOL_VERSION);
        CHECK(fragment.commonHeader.type == static_cast<std::uint8_t>(fm::PacketType::VIDEO_FRAGMENT));
        CHECK(fragment.commonHeader.sessionId == SESSION_ID);
        CHECK(fragment.commonHeader.seq == firstSeq + i);
        CHECK(fragment.commonHeader.timestampUs >= previousTimestampUs);
        previousTimestampUs = fragment.commonHeader.timestampUs;
        CHECK(fragment.frameId == firstFrameId);
        CHECK(fragment.fragIndex == i);
        CHECK(fragment.captureTimestampUs == fm::toTimestampUs(frame.captureTime));

        const auto status = reassembler.accept(fragment, std::chrono::steady_clock::now());
        CHECK(status == (i + 1 == packets.size() ? fm::FragmentStatus::FRAME_COMPLETE : fm::FragmentStatus::ACCEPTED));
    }

    const auto completed = reassembler.completedFrame();
    CHECK(completed.frameId == firstFrameId);
    CHECK(completed.codec == fm::Codec::JPEG);
    CHECK(completed.keyframe);
    CHECK(completed.captureTimestampUs == fm::toTimestampUs(frame.captureTime));
    CHECK(std::ranges::equal(completed.data, data));
}

TEST_CASE("frameId goes up per frame and seq carries on across frames through the wrap") {
    constexpr std::uint32_t firstSeq = 0xFFFF'FFFE;
    constexpr std::uint32_t firstFrameId = 0xFFFF'FFFF;
    constexpr std::size_t frameSize = fm::MAX_FRAGMENT_PAYLOAD + 1;

    Loopback loopback(firstSeq);
    fm::VideoSender videoSender(loopback.sender, firstFrameId);
    const auto firstData = makeFrameData(frameSize);
    auto secondData = makeFrameData(frameSize);
    std::ranges::reverse(secondData);

    REQUIRE(videoSender.send(makeEncodedFrame(firstData, true), fm::Codec::H264, loopback.session).status ==
            fm::FrameSendStatus::FRAME_SENT);
    REQUIRE(videoSender.send(makeEncodedFrame(secondData, false), fm::Codec::H264, loopback.session).status ==
            fm::FrameSendStatus::FRAME_SENT);
    CHECK(loopback.session.nextSeq == 2);

    const auto packets = loopback.receive(4);
    const std::uint32_t expectedSeqs[] = {0xFFFF'FFFE, 0xFFFF'FFFF, 0, 1};
    const std::uint32_t expectedFrameIds[] = {0xFFFF'FFFF, 0xFFFF'FFFF, 0, 0};

    fm::Reassembler reassembler;
    std::vector<std::vector<std::uint8_t>> delivered;
    std::vector<bool> deliveredKeyframes;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        CAPTURE(i);
        const auto fragment = parse(packets[i]);
        CHECK(fragment.commonHeader.seq == expectedSeqs[i]);
        CHECK(fragment.frameId == expectedFrameIds[i]);
        if (reassembler.accept(fragment, std::chrono::steady_clock::now()) == fm::FragmentStatus::FRAME_COMPLETE) {
            const auto completed = reassembler.completedFrame();
            CHECK(completed.codec == fm::Codec::H264);
            delivered.emplace_back(completed.data.begin(), completed.data.end());
            deliveredKeyframes.push_back(completed.keyframe);
        }
    }

    REQUIRE(delivered.size() == 2);
    CHECK(delivered[0] == firstData);
    CHECK(delivered[1] == secondData);
    CHECK(deliveredKeyframes == std::vector<bool>{true, false});
}

TEST_CASE("an invalid frame sends nothing and uses no frameId or seq") {
    constexpr std::uint32_t firstSeq = 10;
    constexpr std::uint32_t firstFrameId = 20;

    Loopback loopback(firstSeq);
    fm::VideoSender videoSender(loopback.sender, firstFrameId);

    SECTION("empty") {
        const std::vector<std::uint8_t> empty;
        const auto result = videoSender.send(makeEncodedFrame(empty, true), fm::Codec::JPEG, loopback.session);
        CHECK(result.status == fm::FrameSendStatus::INVALID_FRAME);
        CHECK(result.packetsSent == 0);
        CHECK(result.bytesSent == 0);
    }
    SECTION("larger than MAX_FRAME_SIZE") {
        const std::vector<std::uint8_t> tooLarge(fm::MAX_FRAME_SIZE + 1);
        const auto result = videoSender.send(makeEncodedFrame(tooLarge, true), fm::Codec::JPEG, loopback.session);
        CHECK(result.status == fm::FrameSendStatus::INVALID_FRAME);
        CHECK(result.packetsSent == 0);
    }
    CHECK(loopback.session.nextSeq == firstSeq);
    CHECK(loopback.nothingMoreArrives());

    const auto data = makeFrameData(10);
    REQUIRE(videoSender.send(makeEncodedFrame(data, true), fm::Codec::JPEG, loopback.session).status ==
            fm::FrameSendStatus::FRAME_SENT);
    const auto fragment = parse(loopback.receive(1)[0]);
    CHECK(fragment.frameId == firstFrameId);
    CHECK(fragment.commonHeader.seq == firstSeq);
}

TEST_CASE("a failed send stops the frame and keeps the seq of the unsent packet") {
    constexpr std::uint32_t firstSeq = 500;
    constexpr std::uint32_t firstFrameId = 40;

    Loopback loopback(firstSeq);
    fm::VideoSender videoSender(loopback.sender, firstFrameId);
    const fm::Endpoint receiverEndpoint = loopback.session.peer;
    const auto data = makeFrameData(3 * fm::MAX_FRAGMENT_PAYLOAD);

    loopback.session.peer = fm::Endpoint{fm::LOOPBACK_IP_ADDRESS, 0};
    const auto result = videoSender.send(makeEncodedFrame(data, true), fm::Codec::JPEG, loopback.session);
    CHECK(result.status == fm::FrameSendStatus::SEND_FAILED);
    CHECK(result.packetsSent == 0);
    CHECK(result.bytesSent == 0);
    CHECK(loopback.session.nextSeq == firstSeq);

    loopback.session.peer = receiverEndpoint;
    REQUIRE(videoSender.send(makeEncodedFrame(data, true), fm::Codec::JPEG, loopback.session).status ==
            fm::FrameSendStatus::FRAME_SENT);
    const auto fragment = parse(loopback.receive(3)[0]);
    CHECK(fragment.frameId == firstFrameId + 1);
    CHECK(fragment.commonHeader.seq == firstSeq);
}
