#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace fm {
    class RateLimiter {
    private:
        using Clock = std::chrono::steady_clock;

        std::vector<Clock::time_point> allowedTimes;
        std::chrono::milliseconds window;
        std::size_t oldest = 0;
        std::size_t used = 0;

    public:
        RateLimiter(std::size_t maxEvents, std::chrono::milliseconds window);

        bool allow(Clock::time_point now) noexcept;
    };
}
