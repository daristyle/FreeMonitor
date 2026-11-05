#pragma once

#include <chrono>
#include <ctime>

namespace fm {
    constexpr std::chrono::seconds MAX_PLAUSIBLE_PACKET_AGE{1};

    constexpr std::chrono::steady_clock::time_point
    arrivalFromKernelTime(std::chrono::steady_clock::time_point readTime, const timespec& wallTime,
                          const timespec& kernelTime) noexcept {
        const auto age = std::chrono::seconds(wallTime.tv_sec - kernelTime.tv_sec) +
                         std::chrono::nanoseconds(wallTime.tv_nsec - kernelTime.tv_nsec);
        if (age <= std::chrono::nanoseconds::zero() || age > MAX_PLAUSIBLE_PACKET_AGE) {
            return readTime;
        }
        return readTime - std::chrono::duration_cast<std::chrono::steady_clock::duration>(age);
    }
}
