#include "CursorShapeCache.hpp"

namespace fm {
    namespace {
        constexpr std::uint32_t argbFromBGRA(const std::uint8_t* pixel) noexcept {
            return (std::uint32_t{pixel[3]} << 24) | (std::uint32_t{pixel[2]} << 16) | (std::uint32_t{pixel[1]} << 8) |
                   std::uint32_t{pixel[0]};
        }
    }

    CursorShapeCache::CursorShapeCache() {
        for (Slot& slot : slots) {
            slot.pixels = std::make_unique<Pixels>();
        }
    }

    std::optional<std::uint8_t> CursorShapeCache::find(std::uint32_t shapeId) const noexcept {
        for (std::size_t index = 0; index < SLOT_COUNT; ++index) {
            if (slots[index].used && slots[index].shapeId == shapeId) {
                return static_cast<std::uint8_t>(index);
            }
        }
        return std::nullopt;
    }

    std::optional<std::uint8_t> CursorShapeCache::store(std::uint32_t shapeId, const CursorShapeInfo& info,
                                                        std::span<const std::uint8_t> bgraPixels,
                                                        std::optional<std::uint8_t> keep) noexcept {
        const std::size_t pixelCount = std::size_t{info.width} * info.height;
        if (pixelCount == 0 || pixelCount > MAX_PIXELS || bgraPixels.size() != pixelCount * BYTES_PER_PIXEL) {
            return std::nullopt;
        }
        const std::uint8_t index = chooseSlot(keep);
        Slot& slot = slots[index];
        const std::uint32_t before = slot.generation.load(std::memory_order_relaxed);
        slot.generation.store(before + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.width.store(info.width, std::memory_order_relaxed);
        slot.height.store(info.height, std::memory_order_relaxed);
        slot.hotspotX.store(info.hotspotX, std::memory_order_relaxed);
        slot.hotspotY.store(info.hotspotY, std::memory_order_relaxed);
        Pixels& pixels = *slot.pixels;
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            pixels[pixel].store(argbFromBGRA(bgraPixels.data() + pixel * BYTES_PER_PIXEL), std::memory_order_relaxed);
        }
        slot.generation.store(before + 2, std::memory_order_release);
        slot.shapeId = shapeId;
        slot.used = true;
        slot.lastUse = ++useCounter;
        return index;
    }

    void CursorShapeCache::touch(std::uint8_t slot) noexcept {
        if (slot < SLOT_COUNT) {
            slots[slot].lastUse = ++useCounter;
        }
    }

    std::uint16_t CursorShapeCache::generation(std::uint8_t slot) const noexcept {
        return slot < SLOT_COUNT ? static_cast<std::uint16_t>(slots[slot].generation.load(std::memory_order_relaxed))
                                 : std::uint16_t{0};
    }

    void CursorShapeCache::clear() noexcept {
        for (Slot& slot : slots) {
            slot.used = false;
        }
    }

    std::optional<CursorShapeInfo> CursorShapeCache::copy(std::uint8_t slot, std::uint16_t generation,
                                                          std::span<std::uint32_t> argbPixels) const noexcept {
        if (slot >= SLOT_COUNT) {
            return std::nullopt;
        }
        const Slot& source = slots[slot];
        const std::uint32_t before = source.generation.load(std::memory_order_acquire);
        if ((before & 1U) != 0 || static_cast<std::uint16_t>(before) != generation) {
            return std::nullopt;
        }
        const CursorShapeInfo info{.width = source.width.load(std::memory_order_relaxed),
                                   .height = source.height.load(std::memory_order_relaxed),
                                   .hotspotX = source.hotspotX.load(std::memory_order_relaxed),
                                   .hotspotY = source.hotspotY.load(std::memory_order_relaxed)};
        const std::size_t pixelCount = std::size_t{info.width} * info.height;
        if (pixelCount == 0 || argbPixels.size() < pixelCount) {
            return std::nullopt;
        }
        const Pixels& pixels = *source.pixels;
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            argbPixels[pixel] = pixels[pixel].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (source.generation.load(std::memory_order_relaxed) != before) {
            return std::nullopt;
        }
        return info;
    }

    std::uint8_t CursorShapeCache::chooseSlot(std::optional<std::uint8_t> keep) const noexcept {
        std::optional<std::uint8_t> oldest;
        for (std::size_t index = 0; index < SLOT_COUNT; ++index) {
            const auto candidate = static_cast<std::uint8_t>(index);
            if (!slots[index].used) {
                return candidate;
            }
            if (candidate != keep && (!oldest || slots[index].lastUse < slots[*oldest].lastUse)) {
                oldest = candidate;
            }
        }
        return oldest.value_or(0);
    }
}
