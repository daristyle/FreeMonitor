#pragma once

#include "ControlPackets.hpp"
#include "ControlTransport.hpp"
#include "HeartbeatEcho.hpp"
#include "PeerTracker.hpp"
#include "ReliableDelivery.hpp"
#include "Session.hpp"
#include "StreamInfo.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace fm {
    enum class TabletState : std::uint8_t {
        IDLE,
        CONNECTING,
        CONNECTED,
        REJECTED,
        ENDED_BY_HOST,
    };

    enum class SessionPacket : std::uint8_t {
        IGNORED,
        CONTROL,
        MEDIA,
    };

    struct TabletSettings {
        Endpoint host;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t refreshHz = 0;
        std::uint8_t codecs = CodecFlags::JPEG | CodecFlags::H264;
        std::uint8_t features = 0;
    };

    class TabletSession {
    private:
        ControlTransport& transport;
        TabletSettings settings;
        std::function<std::uint32_t()> randomNumber;
        TabletState currentState = TabletState::IDLE;
        Session session;
        PeerTracker host;
        ReliableSender reliableSender;
        ReliableReceiver reliableReceiver;
        HeartbeatEcho heartbeatEcho;
        std::optional<std::uint32_t> helloMsgId;
        bool helloAcknowledged = false;
        StreamInfo currentStream;
        std::uint8_t lastRejectReason = 0;
        std::chrono::steady_clock::time_point nextHeartbeatTime{};
        std::chrono::steady_clock::time_point nextStatsTime{};
        bool keyframePending = false;
        std::chrono::steady_clock::time_point nextKeyframeRequestTime{};
        std::uint32_t framesCompleted = 0;
        std::uint32_t framesDropped = 0;
        Session byeSession;
        std::uint32_t byesRemaining = 0;
        std::chrono::steady_clock::time_point nextByeTime{};
        std::uint32_t sessionsStarted = 0;
        std::array<std::uint8_t, MAX_CONTROL_PACKET_SIZE> sendBuffer{};

        bool sessionExists() const noexcept;
        bool acceptWelcome(const Welcome& welcome) const noexcept;
        void handleControl(PacketType type, std::span<const std::uint8_t> packet,
                           std::chrono::steady_clock::time_point now) noexcept;
        void sendPacket(const Session& target, std::size_t size) noexcept;
        void sendAck(std::uint32_t msgId, std::chrono::steady_clock::time_point now) noexcept;
        void sendKeyframeRequest(std::chrono::steady_clock::time_point now) noexcept;
        void sendStats(std::chrono::steady_clock::time_point now) noexcept;
        void sendDueBye(std::chrono::steady_clock::time_point now) noexcept;

    public:
        TabletSession(ControlTransport& transport, TabletSettings settings,
                      std::function<std::uint32_t()> randomNumber);

        TabletSession(const TabletSession&) = delete;
        TabletSession& operator=(const TabletSession&) = delete;

        void start(std::chrono::steady_clock::time_point now) noexcept;
        SessionPacket handlePacket(std::span<const std::uint8_t> packet, const Endpoint& from,
                                   std::chrono::steady_clock::time_point now) noexcept;
        void poll(std::chrono::steady_clock::time_point now) noexcept;
        void close(std::chrono::steady_clock::time_point now) noexcept;
        bool closing() const noexcept;

        void requestKeyframe(std::chrono::steady_clock::time_point now) noexcept;
        void sendShapeRequest(std::uint32_t shapeId, std::chrono::steady_clock::time_point now) noexcept;
        void keyframeReceived() noexcept;
        void frameCompleted() noexcept;
        void frameDropped() noexcept;

        TabletState state() const noexcept;
        std::uint32_t sessionId() const noexcept;
        const StreamInfo& stream() const noexcept;
        std::uint8_t rejectReason() const noexcept;
        std::optional<std::chrono::microseconds> roundTripTime() const noexcept;
        std::uint32_t sessionCount() const noexcept;
    };
}
