#pragma once

#include <cstdint>

namespace fm {
    constexpr std::int32_t serialDifference(std::uint32_t candidate, std::uint32_t reference) noexcept {
        return static_cast<std::int32_t>(candidate - reference);
    }

    constexpr bool isNewer(std::uint32_t candidate, std::uint32_t reference) noexcept {
        return serialDifference(candidate, reference) > 0;
    }
}
