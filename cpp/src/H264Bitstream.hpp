#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace fm {
    std::size_t removeFillerData(std::span<std::uint8_t> accessUnit) noexcept;
}
