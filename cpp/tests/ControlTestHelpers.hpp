#pragma once

#include "ControlPackets.hpp"
#include "ControlTransport.hpp"
#include "HostSessions.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

inline const std::chrono::steady_clock::time_point START = std::chrono::steady_clock::time_point{} + 100s;
inline constexpr fm::Endpoint HOST_ADDRESS{.address = fm::makeIPAddress(10, 0, 0, 1), .port = fm::HOST_PORT};
inline constexpr fm::Endpoint TABLET_ADDRESS{.address = fm::makeIPAddress(10, 0, 0, 2), .port = 40000};
inline constexpr fm::Endpoint OTHER_TABLET_ADDRESS{.address = fm::makeIPAddress(10, 0, 0, 3), .port = 40001};
inline constexpr std::uint8_t BOTH_CODECS = fm::CodecFlags::JPEG | fm::CodecFlags::H264;

struct SentPacket {
    std::vector<std::uint8_t> bytes;
    fm::Endpoint to;

    fm::PacketType type() const {
        return static_cast<fm::PacketType>(bytes[3]);
    }

    fm::CommonHeader header() const {
        fm::CommonHeader header;
        header.read(bytes);
        return header;
    }

    fm::ControlHeader controlHeader() const {
        fm::ControlHeader controlHeader;
        controlHeader.read(bytes);
        return controlHeader;
    }
};

class FakeControlTransport final : public fm::ControlTransport {
public:
    std::vector<SentPacket> sent;

    void send(std::span<const std::uint8_t> packet, const fm::Endpoint& to) noexcept override {
        sent.push_back(SentPacket{{packet.begin(), packet.end()}, to});
    }

    std::vector<SentPacket> ofType(fm::PacketType type) const {
        std::vector<SentPacket> matching;
        for (const SentPacket& packet : sent) {
            if (packet.type() == type) {
                matching.push_back(packet);
            }
        }
        return matching;
    }
};

class RecordingListener final : public fm::HostSessionListener {
public:
    std::vector<std::uint32_t> started;
    std::vector<fm::StreamInfo> streams;
    std::vector<std::uint32_t> streaming;
    std::vector<std::pair<std::uint32_t, fm::SessionEndReason>> ended;
    std::vector<std::pair<std::uint32_t, fm::RejectReason>> rejected;

    void sessionStarted(std::uint32_t sessionId, const fm::TabletInfo&,
                        const fm::StreamInfo& stream) noexcept override {
        started.push_back(sessionId);
        streams.push_back(stream);
    }

    void streamingStarted(std::uint32_t sessionId) noexcept override {
        streaming.push_back(sessionId);
    }

    void sessionEnded(std::uint32_t sessionId, fm::SessionEndReason reason) noexcept override {
        ended.emplace_back(sessionId, reason);
    }

    void helloRejected(std::uint32_t sessionId, fm::RejectReason reason) noexcept override {
        rejected.emplace_back(sessionId, reason);
    }
};

inline std::function<std::uint32_t()> counterFrom(std::uint32_t first) {
    return [next = first]() mutable { return next++; };
}

inline std::vector<std::uint8_t> helloPacket(std::uint32_t sessionId, std::uint32_t seq, std::uint32_t msgId,
                                             std::uint16_t width = 2944, std::uint16_t height = 1840,
                                             std::uint8_t codecs = BOTH_CODECS, std::uint8_t features = 0) {
    fm::Hello hello;
    hello.controlHeader.msgId = msgId;
    hello.width = width;
    hello.height = height;
    hello.refreshHz = 60;
    hello.codecs = codecs;
    hello.features = features;
    std::vector<std::uint8_t> bytes(fm::Hello::SIZE);
    fm::writeControlPacket(hello, sessionId, seq, 1, bytes);
    return bytes;
}

inline std::vector<std::uint8_t> welcomePacket(std::uint32_t sessionId, std::uint32_t seq, std::uint32_t msgId,
                                               fm::Codec codec = fm::Codec::H264, std::uint16_t width = 2944,
                                               std::uint16_t height = 1840, std::uint8_t features = 0) {
    fm::Welcome welcome;
    welcome.controlHeader.msgId = msgId;
    welcome.width = width;
    welcome.height = height;
    welcome.fps = 60;
    welcome.codec = static_cast<std::uint8_t>(codec);
    welcome.features = features;
    std::vector<std::uint8_t> bytes(fm::Welcome::SIZE);
    fm::writeControlPacket(welcome, sessionId, seq, 1, bytes);
    return bytes;
}

inline std::vector<std::uint8_t> ackPacket(std::uint32_t sessionId, std::uint32_t seq, std::uint32_t ackedMsgId) {
    fm::ACK ack;
    ack.ackedMsgId = ackedMsgId;
    std::vector<std::uint8_t> bytes(fm::ACK::SIZE);
    fm::writeControlPacket(ack, sessionId, seq, 1, bytes);
    return bytes;
}

inline std::vector<std::uint8_t> heartbeatPacket(std::uint32_t sessionId, std::uint32_t seq, std::uint64_t timestampUs,
                                                 std::uint64_t echoTimestampUs = 0, std::uint32_t echoDelayUs = 0) {
    fm::Heartbeat heartbeat;
    heartbeat.echoTimestampUs = echoTimestampUs;
    heartbeat.echoDelayUs = echoDelayUs;
    std::vector<std::uint8_t> bytes(fm::Heartbeat::SIZE);
    fm::writeControlPacket(heartbeat, sessionId, seq, timestampUs, bytes);
    return bytes;
}

inline std::vector<std::uint8_t> keyframeRequestPacket(std::uint32_t sessionId, std::uint32_t seq) {
    std::vector<std::uint8_t> bytes(fm::KeyframeRequest::SIZE);
    fm::writeControlPacket(fm::KeyframeRequest{}, sessionId, seq, 1, bytes);
    return bytes;
}

inline std::vector<std::uint8_t> statsPacket(std::uint32_t sessionId, std::uint32_t seq, std::uint32_t packetsReceived,
                                             std::uint32_t packetsLost) {
    fm::Stats stats;
    stats.packetsReceived = packetsReceived;
    stats.packetsLost = packetsLost;
    std::vector<std::uint8_t> bytes(fm::Stats::SIZE);
    fm::writeControlPacket(stats, sessionId, seq, 1, bytes);
    return bytes;
}

inline std::vector<std::uint8_t> byePacket(std::uint32_t sessionId, std::uint32_t seq) {
    std::vector<std::uint8_t> bytes(fm::Bye::SIZE);
    fm::writeControlPacket(fm::Bye{}, sessionId, seq, 1, bytes);
    return bytes;
}

inline std::vector<std::uint8_t> rejectPacket(std::uint32_t sessionId, std::uint8_t reason,
                                              std::uint8_t version = fm::PROTOCOL_VERSION) {
    fm::Reject reject;
    reject.reason = reason;
    reject.version = fm::PROTOCOL_VERSION;
    std::vector<std::uint8_t> bytes(fm::Reject::SIZE);
    fm::writeControlPacket(reject, sessionId, 0, 1, bytes);
    bytes[2] = version;
    return bytes;
}

inline std::uint64_t timestampOf(std::chrono::steady_clock::time_point time) {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count());
}
