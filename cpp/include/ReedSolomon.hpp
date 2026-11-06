#pragma once

#include "Packets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fm {
    constexpr std::uint32_t MIN_FRAME_PARITY = 2;
    constexpr std::uint32_t FULL_PERCENT = 100;

    struct ParityBlock {
        std::uint16_t parityIndex = 0;
        std::span<std::uint8_t> bytes;
    };

    constexpr std::uint16_t fecGroupCount(std::uint16_t fragCount) noexcept {
        return static_cast<std::uint16_t>((fragCount + MAX_FEC_GROUP_SIZE - 1) / MAX_FEC_GROUP_SIZE);
    }

    constexpr std::size_t fecBlockSize(std::uint32_t frameSize) noexcept {
        return std::min(frameSize, MAX_FRAGMENT_PAYLOAD);
    }

    constexpr std::uint16_t fecParityCount(std::uint16_t fragCount, std::uint8_t percent) noexcept {
        if (fragCount == 0 || percent == 0) {
            return 0;
        }
        const std::uint32_t wanted = (std::uint32_t{fragCount} * percent + FULL_PERCENT - 1) / FULL_PERCENT;
        const std::uint32_t most = std::uint32_t{fecGroupCount(fragCount)} * MAX_FEC_GROUP_PARITY;
        return static_cast<std::uint16_t>(std::clamp(wanted, MIN_FRAME_PARITY, most));
    }

    std::uint8_t gfMultiply(std::uint8_t left, std::uint8_t right) noexcept;
    std::uint8_t gfInverse(std::uint8_t value) noexcept;
    std::uint8_t parityCoefficient(std::size_t row, std::size_t column) noexcept;

    bool computeParity(std::span<const std::uint8_t> frameData, std::uint16_t parityIndex,
                       std::span<std::uint8_t> parity) noexcept;
    bool recoverFragments(std::span<std::uint8_t> frameData, std::span<const std::uint16_t> missingFragments,
                          std::span<const ParityBlock> parityBlocks) noexcept;
}
