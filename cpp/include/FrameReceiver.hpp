#pragma once

#include "Error.hpp"
#include "FrameMailbox.hpp"
#include "Packets.hpp"
#include "Reassembler.hpp"
#include "StreamInfo.hpp"
#include "TabletSession.hpp"
#include "UDPControlTransport.hpp"
#include "UDPSocket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <thread>

namespace fm {
    struct FrameReceiverStatistics {
        std::uint64_t packets = 0;
        std::uint64_t invalidPackets = 0;
        std::uint64_t ignoredPackets = 0;
        std::uint64_t framesCompleted = 0;
        std::uint64_t framesDropped = 0;
        std::uint64_t framesReplaced = 0;
        std::uint64_t senderRestarts = 0;
    };

    struct FrameReceiverSessionStatus {
        TabletState state = TabletState::IDLE;
        std::uint8_t rejectReason = 0;
        std::uint32_t sessionId = 0;
        std::optional<std::chrono::microseconds> roundTripTime;
        StreamInfo stream;
        std::uint32_t sessionCount = 0;
    };

    class FrameReceiver {
    private:
        static constexpr std::chrono::milliseconds RECEIVE_POLL_INTERVAL{100};
        static constexpr std::chrono::milliseconds SESSION_POLL_INTERVAL{10};
        static constexpr std::chrono::milliseconds BYE_WAIT_LIMIT{200};
        static constexpr std::chrono::milliseconds BYE_WAIT_STEP{5};
        static constexpr int RECEIVE_BUFFER_SIZE = 8 * 1024 * 1024;

        UDPSocket udpSocket;
        UDPControlTransport controlTransport;
        std::random_device randomDevice;
        std::optional<TabletSession> session;
        std::uint32_t sessionId;
        std::chrono::milliseconds senderRestartTimeout;
        Reassembler reassembler;
        FrameMailbox<ReassembledFrame> mailbox;
        std::array<std::uint8_t, MAX_PACKET_SIZE> packetBuffer{};
        std::chrono::steady_clock::time_point lastCompletion;
        std::uint64_t framesDroppedBeforeReset = 0;
        std::uint64_t framesDroppedReported = 0;
        std::uint32_t lastSessionCount = 0;
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> keyframeWanted{false};
        std::atomic<bool> reconnectWanted{false};
        std::atomic<std::uint64_t> packets{0};
        std::atomic<std::uint64_t> invalidPackets{0};
        std::atomic<std::uint64_t> ignoredPackets{0};
        std::atomic<std::uint64_t> framesCompleted{0};
        std::atomic<std::uint64_t> framesDropped{0};
        std::atomic<std::uint64_t> senderRestarts{0};
        std::atomic<TabletState> statusState{TabletState::IDLE};
        std::atomic<std::uint8_t> statusRejectReason{0};
        std::atomic<std::uint32_t> statusSessionId{0};
        std::atomic<std::int64_t> statusRoundTripUs{-1};
        std::atomic<std::uint16_t> statusWidth{0};
        std::atomic<std::uint16_t> statusHeight{0};
        std::atomic<std::uint16_t> statusFps{0};
        std::atomic<Codec> statusCodec{Codec::JPEG};
        std::atomic<std::uint8_t> statusFeatures{0};
        std::atomic<std::uint32_t> statusSessionCount{0};
        std::thread receiveThread;

        FrameReceiver(UDPSocket udpSocket, std::uint32_t sessionId, std::chrono::milliseconds senderRestartTimeout,
                      const std::optional<TabletSettings>& settings);

        void run() noexcept;
        void handlePacket(std::span<const std::uint8_t> packet, const Endpoint& from,
                          std::chrono::steady_clock::time_point arrival) noexcept;
        void publishCompletedFrame(std::chrono::steady_clock::time_point now) noexcept;
        void resetReassembler() noexcept;
        void noteDroppedFrames(std::chrono::steady_clock::time_point now) noexcept;
        void pollSession(std::chrono::steady_clock::time_point now) noexcept;
        void closeSession() noexcept;
        void publishSessionStatus() noexcept;

    public:
        static constexpr std::chrono::milliseconds DEFAULT_SENDER_RESTART_TIMEOUT{SESSION_TIMEOUT_MS};

        static Result<std::unique_ptr<FrameReceiver>>
        open(const Endpoint& bindTo, std::uint32_t sessionId,
             std::chrono::milliseconds senderRestartTimeout = DEFAULT_SENDER_RESTART_TIMEOUT);
        static Result<std::unique_ptr<FrameReceiver>> openSession(const TabletSettings& settings);

        ~FrameReceiver();

        FrameReceiver(const FrameReceiver&) = delete;
        FrameReceiver& operator=(const FrameReceiver&) = delete;

        Result<Endpoint> localEndpoint() const;
        void start();
        void stop() noexcept;
        MailboxStatus acquire(ReassembledFrame& frame, std::chrono::milliseconds timeout) noexcept;
        void release() noexcept;
        FrameReceiverStatistics statistics() noexcept;
        void requestKeyframe() noexcept;
        void reconnect() noexcept;
        FrameReceiverSessionStatus sessionStatus() const noexcept;
    };
}
