#pragma once

#include "Error.hpp"
#include "FrameMailbox.hpp"
#include "Packets.hpp"
#include "Reassembler.hpp"
#include "UDPSocket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
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

    class FrameReceiver {
    private:
        static constexpr std::chrono::milliseconds RECEIVE_POLL_INTERVAL{100};
        static constexpr int RECEIVE_BUFFER_SIZE = 8 * 1024 * 1024;

        UDPSocket udpSocket;
        std::uint32_t sessionId;
        std::chrono::milliseconds senderRestartTimeout;
        Reassembler reassembler;
        FrameMailbox<ReassembledFrame> mailbox;
        std::array<std::uint8_t, MAX_PACKET_SIZE> packetBuffer{};
        std::chrono::steady_clock::time_point lastCompletion;
        std::uint64_t framesDroppedBeforeReset = 0;
        std::atomic<bool> stopRequested{false};
        std::atomic<std::uint64_t> packets{0};
        std::atomic<std::uint64_t> invalidPackets{0};
        std::atomic<std::uint64_t> ignoredPackets{0};
        std::atomic<std::uint64_t> framesCompleted{0};
        std::atomic<std::uint64_t> framesDropped{0};
        std::atomic<std::uint64_t> senderRestarts{0};
        std::thread receiveThread;

        FrameReceiver(UDPSocket udpSocket, std::uint32_t sessionId, std::chrono::milliseconds senderRestartTimeout);

        void run() noexcept;
        void handlePacket(std::span<const std::uint8_t> packet, std::chrono::steady_clock::time_point now) noexcept;
        void publishCompletedFrame(std::chrono::steady_clock::time_point now) noexcept;
        void resetReassembler() noexcept;

    public:
        static constexpr std::chrono::milliseconds DEFAULT_SENDER_RESTART_TIMEOUT{SESSION_TIMEOUT_MS};

        static Result<std::unique_ptr<FrameReceiver>> open(const Endpoint& bindTo, std::uint32_t sessionId,
             std::chrono::milliseconds senderRestartTimeout = DEFAULT_SENDER_RESTART_TIMEOUT);

        ~FrameReceiver();

        FrameReceiver(const FrameReceiver&) = delete;
        FrameReceiver& operator=(const FrameReceiver&) = delete;

        Result<Endpoint> localEndpoint() const;
        void start();
        void stop() noexcept;
        MailboxStatus acquire(ReassembledFrame& frame, std::chrono::milliseconds timeout) noexcept;
        void release() noexcept;
        FrameReceiverStatistics statistics() noexcept;
    };
}
