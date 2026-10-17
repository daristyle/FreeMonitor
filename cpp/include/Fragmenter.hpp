#pragma once

#include "Packets.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fm {
    class Fragmenter {
    private:
        std::span<const std::uint8_t> frameData;
        VideoFragment fragment;

    public:
        bool begin(std::span<const std::uint8_t> frameData, std::uint32_t frameId, Codec codec, bool keyframe,
                   std::uint64_t captureTimestampUs) noexcept;
        std::uint16_t fragmentCount() const noexcept;
        std::size_t write(std::uint16_t fragIndex, std::uint32_t sessionId, std::uint32_t seq,
                          std::uint64_t timestampUs, std::span<std::uint8_t> packetBuffer) const noexcept;
    };
}
