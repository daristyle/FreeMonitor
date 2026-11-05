#pragma once

#include "Packets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fm {
    constexpr std::size_t MAX_CONTROL_PACKET_SIZE =
            std::max({Hello::SIZE, Welcome::SIZE, ACK::SIZE, Heartbeat::SIZE, KeyframeRequest::SIZE, ShapeRequest::SIZE,
                      Stats::SIZE, Bye::SIZE, Reject::SIZE});

    enum class ControlPacketCheck : std::uint8_t {
        VALID,
        INVALID,
        UNSUPPORTED_VERSION,
    };

    ControlPacketCheck checkControlPacket(std::span<const std::uint8_t> packet) noexcept;
    bool isMediaPacket(const CommonHeader& header) noexcept;
    bool isReliable(PacketType type) noexcept;
    void restampHeader(std::span<std::uint8_t> packet, std::uint32_t seq, std::uint64_t timestampUs) noexcept;

    template<typename Message>
    std::size_t writeControlPacket(Message message, std::uint32_t sessionId, std::uint32_t seq,
                                   std::uint64_t timestampUs, std::span<std::uint8_t> buffer) noexcept;
}

#include "ControlPackets.tpp"
