#pragma once

#include "ControlPackets.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fm {
    class ReliableSender {
    private:
        static constexpr std::size_t QUEUE_CAPACITY = 4;

        struct Message {
            std::array<std::uint8_t, MAX_CONTROL_PACKET_SIZE> bytes{};
            std::size_t size = 0;
            std::uint32_t msgId = 0;
        };

        std::array<Message, QUEUE_CAPACITY> queue{};
        std::size_t first = 0;
        std::size_t count = 0;
        std::uint32_t nextMsgId = 1;
        std::chrono::steady_clock::time_point nextSendTime{};

    public:
        void reset() noexcept;
        std::optional<std::uint32_t> enqueue(std::span<const std::uint8_t> packet,
                                             std::chrono::steady_clock::time_point now) noexcept;
        std::span<std::uint8_t> due(std::chrono::steady_clock::time_point now,
                                    std::chrono::milliseconds resendInterval) noexcept;
        bool acknowledge(std::uint32_t msgId, std::chrono::steady_clock::time_point now) noexcept;
        bool empty() const noexcept;
    };

    class ReliableReceiver {
    private:
        std::uint32_t lastProcessedMsgId = 0;
        bool processedAny = false;

    public:
        void reset() noexcept;
        bool shouldProcess(std::uint32_t msgId) noexcept;
    };
}
