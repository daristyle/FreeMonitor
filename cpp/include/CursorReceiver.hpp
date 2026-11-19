#pragma once

#include "CursorShapeCache.hpp"
#include "Packets.hpp"

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fm {
    struct CursorSnapshot {
        static constexpr std::uint8_t NO_SLOT = 0xFF;

        std::int16_t x = 0;
        std::int16_t y = 0;
        bool visible = false;
        std::uint8_t slot = NO_SLOT;
        std::uint16_t generation = 0;

        std::uint64_t pack() const noexcept;
        static CursorSnapshot unpack(std::uint64_t packed) noexcept;

        friend bool operator==(const CursorSnapshot&, const CursorSnapshot&) = default;
    };

    class CursorReceiver {
    private:
        static constexpr std::size_t MAX_SHAPE_BYTES = CursorShapeCache::MAX_PIXELS * CursorShapeCache::BYTES_PER_PIXEL;
        static constexpr std::size_t MAX_SHAPE_FRAGMENTS =
                (MAX_SHAPE_BYTES + MAX_SHAPE_PAYLOAD - 1) / MAX_SHAPE_PAYLOAD;
        static constexpr std::chrono::milliseconds FRAGMENT_TIMEOUT{FRAGMENT_TIMEOUT_MS};
        static constexpr std::chrono::milliseconds SHAPE_REQUEST_REPEAT_INTERVAL{SHAPE_REQUEST_REPEAT_INTERVAL_MS};

        CursorShapeCache cache;
        std::atomic<std::uint64_t> published;

        std::optional<std::uint32_t> lastPositionSeq;
        std::int32_t x = 0;
        std::int32_t y = 0;
        bool visible = false;
        std::uint32_t shapeId = 0;
        std::optional<std::uint8_t> drawnSlot;
        std::optional<std::uint32_t> missingShapeId;
        std::optional<std::chrono::steady_clock::time_point> lastRequestTime;

        bool assembling = false;
        std::uint32_t assemblyShapeId = 0;
        std::uint32_t assemblySeq = 0;
        std::uint16_t assemblyFragCount = 0;
        std::uint16_t assemblyFragmentsReceived = 0;
        CursorShapeInfo assemblyInfo;
        std::chrono::steady_clock::time_point lastFragmentTime{};
        std::bitset<MAX_SHAPE_FRAGMENTS> assemblyReceived;
        std::vector<std::uint8_t> assemblyBytes;

        void expire(std::chrono::steady_clock::time_point now) noexcept;
        void startAssembly(const CursorShape& fragment) noexcept;
        void completeAssembly() noexcept;
        void useSlot(std::uint8_t slot) noexcept;
        void publish() noexcept;

    public:
        CursorReceiver();

        CursorReceiver(const CursorReceiver&) = delete;
        CursorReceiver& operator=(const CursorReceiver&) = delete;

        void acceptPosition(const CursorPos& position) noexcept;
        bool acceptShapeFragment(const CursorShape& fragment, std::chrono::steady_clock::time_point now) noexcept;
        std::optional<std::uint32_t> takeShapeRequest(std::chrono::steady_clock::time_point now) noexcept;
        void reset() noexcept;

        std::uint64_t snapshot() const noexcept;
        std::optional<CursorShapeInfo> copyShape(std::uint64_t packedSnapshot,
                                                 std::span<std::uint32_t> argbPixels) const noexcept;
    };
}
