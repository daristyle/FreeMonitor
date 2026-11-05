#pragma once

#include "Packets.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace fm {
    class HeartbeatEcho {
    private:
        std::uint64_t peerTimestampUs = 0;
        std::chrono::steady_clock::time_point receivedTime{};
        std::optional<std::chrono::microseconds> lastRoundTripTime;

    public:
        void reset() noexcept;
        void received(const Heartbeat& heartbeat, std::chrono::steady_clock::time_point now) noexcept;
        Heartbeat nextHeartbeat(std::chrono::steady_clock::time_point now) const noexcept;
        std::optional<std::chrono::microseconds> roundTripTime() const noexcept;
    };
}
