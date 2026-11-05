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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace fm {
    struct TabletInfo {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t refreshHz = 0;
        std::uint8_t codecs = 0;
        std::uint8_t features = 0;
    };

    struct HostOffer {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t fps = 0;
        std::uint8_t codecs = CodecFlags::JPEG;
        std::uint8_t features = 0;
    };

    struct TabletReport {
        std::optional<Stats> stats;
        std::optional<ClockSample> clockSample;
    };

    struct StatsDifference {
        std::chrono::microseconds interval{0};
        std::uint32_t packetsReceived = 0;
        std::int32_t packetsLost = 0;
        std::uint32_t framesCompleted = 0;
        std::uint32_t framesDropped = 0;
        std::uint32_t bytesReceived = 0;
    };

    enum class SessionEndReason : std::uint8_t {
        REPLACED,
        BYE,
        TIMEOUT,
        CLOSED,
    };

    class HostSessionListener {
    public:
        virtual ~HostSessionListener() = default;

        virtual void sessionStarted(std::uint32_t sessionId, const TabletInfo& tablet,
                                    const StreamInfo& stream) noexcept = 0;
        virtual void streamingStarted(std::uint32_t sessionId) noexcept = 0;
        virtual void sessionEnded(std::uint32_t sessionId, SessionEndReason reason) noexcept = 0;
        virtual void helloRejected(std::uint32_t sessionId, RejectReason reason) noexcept = 0;
    };

    class HostSessions {
    private:
        static constexpr std::size_t MAX_PENDING_BYES = 4;

        struct Client {
            Session session;
            PeerTracker peer;
            ReliableSender reliableSender;
            ReliableReceiver reliableReceiver;
            TabletInfo tablet;
            StreamInfo stream;
            std::optional<std::uint32_t> welcomeMsgId;
            bool streaming = false;
            bool keyframeRequested = false;
            std::optional<std::chrono::steady_clock::time_point> lastKeyframeTime;
            std::chrono::steady_clock::time_point nextHeartbeatTime{};
            HeartbeatEcho heartbeatEcho;
            std::optional<Stats> lastStats;
        };

        struct PendingBye {
            Session session;
            std::uint32_t remaining = 0;
            std::chrono::steady_clock::time_point nextSendTime{};
        };

        ControlTransport& transport;
        HostSessionListener& listener;
        HostOffer offer;
        std::function<std::uint32_t()> randomNumber;
        std::size_t maxSessions;
        std::vector<Client> clients;
        std::array<PendingBye, MAX_PENDING_BYES> pendingByes{};
        std::array<std::uint8_t, MAX_CONTROL_PACKET_SIZE> sendBuffer{};

        Client* findClient(std::uint32_t sessionId) noexcept;
        Client* streamingClient() noexcept;
        const Client* streamingClient() const noexcept;
        void handleHello(std::span<const std::uint8_t> packet, const Endpoint& from,
                         std::chrono::steady_clock::time_point now) noexcept;
        void handleSessionPacket(std::size_t clientIndex, PacketType type, const CommonHeader& header,
                                 std::span<const std::uint8_t> packet, const Endpoint& from,
                                 std::chrono::steady_clock::time_point now) noexcept;
        void reject(std::uint32_t sessionId, RejectReason reason, const Endpoint& to,
                    std::chrono::steady_clock::time_point now) noexcept;
        void endSession(std::size_t clientIndex, SessionEndReason reason, bool sendBye,
                        std::chrono::steady_clock::time_point now) noexcept;
        void sendPacket(Session& session, std::size_t size) noexcept;
        void sendAck(Client& client, std::uint32_t msgId, std::chrono::steady_clock::time_point now) noexcept;
        void sendHeartbeat(Client& client, std::chrono::steady_clock::time_point now) noexcept;
        void sendDueByes(std::chrono::steady_clock::time_point now) noexcept;

    public:
        HostSessions(ControlTransport& transport, HostSessionListener& listener, HostOffer offer,
                     std::function<std::uint32_t()> randomNumber, std::size_t maxSessions = 1);

        HostSessions(const HostSessions&) = delete;
        HostSessions& operator=(const HostSessions&) = delete;

        void setOffer(const HostOffer& newOffer) noexcept;
        void handlePacket(std::span<const std::uint8_t> packet, const Endpoint& from,
                          std::chrono::steady_clock::time_point now) noexcept;
        void poll(std::chrono::steady_clock::time_point now) noexcept;

        Session* streamingSession() noexcept;
        bool takeKeyframeRequest() noexcept;
        void keyframeSent(std::chrono::steady_clock::time_point now) noexcept;
        std::optional<TabletReport> report() const noexcept;

        void close(std::chrono::steady_clock::time_point now) noexcept;
        bool closing() const noexcept;
    };

    std::optional<StatsDifference> statsDifference(const Stats& earlier, const Stats& later) noexcept;
}
