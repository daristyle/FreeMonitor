#include "RateLimiter.hpp"

namespace fm {
    RateLimiter::RateLimiter(std::size_t maxEvents, std::chrono::milliseconds window)
        : allowedTimes(maxEvents), window(window) {}

    bool RateLimiter::allow(Clock::time_point now) noexcept {
        if (allowedTimes.empty()) {
            return false;
        }
        if (used < allowedTimes.size()) {
            allowedTimes[(oldest + used) % allowedTimes.size()] = now;
            ++used;
            return true;
        }
        if (now - allowedTimes[oldest] < window) {
            return false;
        }
        allowedTimes[oldest] = now;
        oldest = (oldest + 1) % allowedTimes.size();
        return true;
    }
}
