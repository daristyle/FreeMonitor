#pragma once

#include <chrono>
#include <cstdint>

namespace fm {
    constexpr std::uint64_t toTimestampUs(std::chrono::steady_clock::time_point time) noexcept {
        return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count());
    }
}
