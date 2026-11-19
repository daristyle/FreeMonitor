#pragma once

#include "CursorSource.hpp"
#include "Packets.hpp"
#include "Session.hpp"
#include "UDPSocket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace fm {
    struct CursorTarget {
        Endpoint peer;
        std::uint32_t sessionId = 0;
        SequenceCounter seq;
    };

    struct CursorSendCounts {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
    };

    class CursorSender {
    private:
        using Clock = std::chrono::steady_clock;

        UDPSocket& udpSocket;
        CursorSource& source;
        std::optional<std::chrono::milliseconds> shapeRepeatInterval;

        std::mutex targetMutex;
        std::optional<CursorTarget> pendingTarget;
        std::atomic<bool> targetChanged{false};
        std::atomic<std::uint32_t> requestedShape{0};
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> sourceLost{false};
        std::atomic<std::uint64_t> packetsSent{0};
        std::atomic<std::uint64_t> bytesSent{0};

        std::optional<CursorTarget> target;
        CursorState latest;
        CursorState sent;
        bool positionSent = false;
        std::uint32_t sentShapeId = 0;
        Clock::time_point lastPositionTime{};
        Clock::time_point lastShapeTime{};
        std::array<std::uint8_t, MAX_PACKET_SIZE> packetBuffer{};
        std::thread thread;

        void run() noexcept;
        bool positionPending() const noexcept;
        void sendShape(Clock::time_point now) noexcept;
        void sendPosition(Clock::time_point now) noexcept;
        bool sendPacket(std::size_t size) noexcept;

    public:
        static constexpr std::chrono::milliseconds MIN_POSITION_INTERVAL{CURSOR_POS_MIN_INTERVAL_MS};
        static constexpr std::chrono::milliseconds POSITION_REPEAT_INTERVAL{CURSOR_POS_REPEAT_INTERVAL_MS};

        CursorSender(UDPSocket& udpSocket, CursorSource& source,
                     std::optional<std::chrono::milliseconds> shapeRepeatInterval = std::nullopt);
        ~CursorSender();

        CursorSender(const CursorSender&) = delete;
        CursorSender& operator=(const CursorSender&) = delete;

        void start();
        void stop() noexcept;
        void setTarget(std::optional<CursorTarget> newTarget);
        void requestShape(std::uint32_t shapeId) noexcept;
        bool lost() const noexcept;
        CursorSendCounts counts() const noexcept;

        void update(const CursorState& state) noexcept;
        Clock::time_point sendDue(Clock::time_point now) noexcept;
    };
}
