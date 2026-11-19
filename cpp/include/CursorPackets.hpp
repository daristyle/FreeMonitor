#pragma once
#pragma once

#include "CursorSource.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fm {
    std::size_t writeCursorPos(const CursorState& state, std::uint32_t sessionId, std::uint32_t seq,
                               std::uint64_t timestampUs, std::span<std::uint8_t> buffer) noexcept;

    std::uint16_t cursorShapeFragmentCount(const CursorImage& image) noexcept;

    std::size_t writeCursorShapeFragment(const CursorImage& image, std::uint32_t shapeId, std::uint16_t fragIndex,
                                         std::uint32_t sessionId, std::uint32_t seq, std::uint64_t timestampUs,
                                         std::span<std::uint8_t> buffer) noexcept;
}
