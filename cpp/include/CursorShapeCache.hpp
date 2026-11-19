#pragma once

#include "CursorSource.hpp"
#include "Packets.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace fm {
    struct CursorShapeInfo {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t hotspotX = 0;
        std::uint16_t hotspotY = 0;

        friend bool operator==(const CursorShapeInfo&, const CursorShapeInfo&) = default;
    };

    class CursorShapeCache {
    private:
        using Pixels =
                std::array<std::atomic<std::uint32_t>, std::size_t{MAX_CURSOR_IMAGE_SIDE} * MAX_CURSOR_IMAGE_SIDE>;

        struct Slot {
            std::atomic<std::uint32_t> generation{0};
            std::atomic<std::uint16_t> width{0};
            std::atomic<std::uint16_t> height{0};
            std::atomic<std::uint16_t> hotspotX{0};
            std::atomic<std::uint16_t> hotspotY{0};
            std::unique_ptr<Pixels> pixels;
            std::uint32_t shapeId = 0;
            bool used = false;
            std::uint64_t lastUse = 0;
        };

        std::array<Slot, MIN_SHAPE_CACHE_SIZE> slots;
        std::uint64_t useCounter = 0;

        std::uint8_t chooseSlot(std::optional<std::uint8_t> keep) const noexcept;

    public:
        static constexpr std::size_t SLOT_COUNT = MIN_SHAPE_CACHE_SIZE;
        static constexpr std::size_t MAX_PIXELS = std::tuple_size_v<Pixels>;
        static constexpr std::size_t BYTES_PER_PIXEL = 4;

        CursorShapeCache();

        CursorShapeCache(const CursorShapeCache&) = delete;
        CursorShapeCache& operator=(const CursorShapeCache&) = delete;

        std::optional<std::uint8_t> find(std::uint32_t shapeId) const noexcept;
        std::optional<std::uint8_t> store(std::uint32_t shapeId, const CursorShapeInfo& info,
                                          std::span<const std::uint8_t> bgraPixels,
                                          std::optional<std::uint8_t> keep) noexcept;
        void touch(std::uint8_t slot) noexcept;
        std::uint16_t generation(std::uint8_t slot) const noexcept;
        void clear() noexcept;

        std::optional<CursorShapeInfo> copy(std::uint8_t slot, std::uint16_t generation,
                                            std::span<std::uint32_t> argbPixels) const noexcept;
    };
}
