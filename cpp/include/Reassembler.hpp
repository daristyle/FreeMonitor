#pragma once

#include "Packets.hpp"

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fm {
    constexpr std::uint32_t MAX_FRAGMENT_COUNT = (MAX_FRAME_SIZE + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD;

    constexpr std::optional<Codec> toCodec(std::uint8_t value) noexcept {
        switch (value) {
            case static_cast<std::uint8_t>(Codec::JPEG):
                return Codec::JPEG;
            case static_cast<std::uint8_t>(Codec::H264):
                return Codec::H264;
            default:
                return std::nullopt;
        }
    }

    enum class FragmentStatus : std::uint8_t {
        ACCEPTED,
        FRAME_COMPLETE,
        DUPLICATE,
        TOO_OLD,
        INVALID,
    };

    struct ReassembledFrame {
        std::span<const std::uint8_t> data;
        std::uint32_t frameId = 0;
        Codec codec = Codec::JPEG;
        bool keyframe = false;
        std::uint64_t captureTimestampUs = 0;
    };

    class Reassembler {
    private:
        struct Slot {
            std::vector<std::uint8_t> data;
            std::bitset<MAX_FRAGMENT_COUNT> received;
            std::chrono::steady_clock::time_point lastProgress;
            std::uint64_t captureTimestampUs = 0;
            std::uint32_t frameId = 0;
            std::uint32_t frameSize = 0;
            std::uint16_t fragCount = 0;
            std::uint16_t receivedCount = 0;
            Codec codec = Codec::JPEG;
            std::uint8_t flags = 0;
            bool inUse = false;
        };

        std::array<Slot, MAX_INCOMPLETE_FRAMES> slots;
        ReassembledFrame completed;
        std::optional<std::uint32_t> lastDeliveredFrameId;
        std::uint32_t completedCount = 0;
        std::uint32_t droppedCount = 0;

        static bool matches(const Slot& slot, const VideoFragment& fragment) noexcept;
        static void start(Slot& slot, const VideoFragment& fragment) noexcept;
        Slot* findSlot(std::uint32_t frameId) noexcept;
        Slot* claimSlot(std::uint32_t frameId) noexcept;
        void discard(Slot& slot) noexcept;
        void deliver(Slot& slot) noexcept;

    public:
        Reassembler();

        Reassembler(const Reassembler&) = delete;
        Reassembler& operator=(const Reassembler&) = delete;

        FragmentStatus accept(const VideoFragment& fragment, std::chrono::steady_clock::time_point now) noexcept;
        void expire(std::chrono::steady_clock::time_point now) noexcept;
        void reset() noexcept;
        ReassembledFrame completedFrame() const noexcept;
        std::uint32_t framesCompleted() const noexcept;
        std::uint32_t framesDropped() const noexcept;
    };
}
