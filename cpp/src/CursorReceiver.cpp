#include "CursorReceiver.hpp"

#include "SerialNumber.hpp"

#include <algorithm>
#include <limits>

namespace fm {
    namespace {
        constexpr unsigned Y_SHIFT = 16;
        constexpr unsigned SLOT_SHIFT = 32;
        constexpr unsigned VISIBLE_SHIFT = 40;
        constexpr unsigned GENERATION_SHIFT = 48;
        constexpr std::uint64_t FIELD_16_BITS = 0xFFFF;
        constexpr std::uint64_t FIELD_8_BITS = 0xFF;

        std::int16_t clampToInt16(std::int32_t value) noexcept {
            return static_cast<std::int16_t>(std::clamp<std::int32_t>(value, std::numeric_limits<std::int16_t>::min(),
                                                                      std::numeric_limits<std::int16_t>::max()));
        }

        std::size_t shapeBytes(const CursorShape& fragment) noexcept {
            return std::size_t{fragment.width} * fragment.height * CursorShapeCache::BYTES_PER_PIXEL;
        }

        bool followsShapeRules(const CursorShape& fragment) noexcept {
            if (fragment.width < 1 || fragment.width > MAX_CURSOR_IMAGE_SIDE || fragment.height < 1 ||
                fragment.height > MAX_CURSOR_IMAGE_SIDE || fragment.hotspotX >= fragment.width ||
                fragment.hotspotY >= fragment.height) {
                return false;
            }
            const std::size_t totalBytes = shapeBytes(fragment);
            const std::size_t fragCount = (totalBytes + MAX_SHAPE_PAYLOAD - 1) / MAX_SHAPE_PAYLOAD;
            if (fragment.fragCount != fragCount || fragment.fragIndex >= fragment.fragCount) {
                return false;
            }
            const std::size_t offset = std::size_t{fragment.fragIndex} * MAX_SHAPE_PAYLOAD;
            const std::size_t expectedSize = std::min<std::size_t>(MAX_SHAPE_PAYLOAD, totalBytes - offset);
            return fragment.payload.size() == expectedSize;
        }

        CursorShapeInfo infoOf(const CursorShape& fragment) noexcept {
            return {.width = fragment.width,
                    .height = fragment.height,
                    .hotspotX = fragment.hotspotX,
                    .hotspotY = fragment.hotspotY};
        }
    }

    std::uint64_t CursorSnapshot::pack() const noexcept {
        return std::uint64_t{static_cast<std::uint16_t>(x)} |
               (std::uint64_t{static_cast<std::uint16_t>(y)} << Y_SHIFT) | (std::uint64_t{slot} << SLOT_SHIFT) |
               (std::uint64_t{visible ? 1U : 0U} << VISIBLE_SHIFT) | (std::uint64_t{generation} << GENERATION_SHIFT);
    }

    CursorSnapshot CursorSnapshot::unpack(std::uint64_t packed) noexcept {
        return {.x = static_cast<std::int16_t>(static_cast<std::uint16_t>(packed & FIELD_16_BITS)),
                .y = static_cast<std::int16_t>(static_cast<std::uint16_t>((packed >> Y_SHIFT) & FIELD_16_BITS)),
                .visible = ((packed >> VISIBLE_SHIFT) & 1U) != 0,
                .slot = static_cast<std::uint8_t>((packed >> SLOT_SHIFT) & FIELD_8_BITS),
                .generation = static_cast<std::uint16_t>((packed >> GENERATION_SHIFT) & FIELD_16_BITS)};
    }

    CursorReceiver::CursorReceiver() : published(CursorSnapshot{}.pack()), assemblyBytes(MAX_SHAPE_BYTES) {}

    void CursorReceiver::acceptPosition(const CursorPos& position) noexcept {
        const std::uint32_t seq = position.commonHeader.seq;
        if (lastPositionSeq && !isNewer(seq, *lastPositionSeq)) {
            return;
        }
        lastPositionSeq = seq;
        x = position.x;
        y = position.y;
        visible = (position.flags & CursorFlags::VISIBLE) != 0;
        shapeId = position.shapeId;
        if (const auto slot = cache.find(shapeId)) {
            useSlot(*slot);
        } else {
            missingShapeId = shapeId;
        }
        publish();
    }

    bool CursorReceiver::acceptShapeFragment(const CursorShape& fragment,
                                             std::chrono::steady_clock::time_point now) noexcept {
        if (!followsShapeRules(fragment)) {
            return false;
        }
        if (cache.find(fragment.shapeId)) {
            return true;
        }
        expire(now);
        if (assembling && fragment.shapeId == assemblyShapeId) {
            if (infoOf(fragment) != assemblyInfo) {
                return false;
            }
        } else if (assembling && !isNewer(fragment.commonHeader.seq, assemblySeq)) {
            return true;
        } else {
            startAssembly(fragment);
        }
        lastFragmentTime = now;
        if (isNewer(fragment.commonHeader.seq, assemblySeq)) {
            assemblySeq = fragment.commonHeader.seq;
        }
        if (!assemblyReceived[fragment.fragIndex]) {
            assemblyReceived[fragment.fragIndex] = true;
            ++assemblyFragmentsReceived;
            std::ranges::copy(fragment.payload,
                              assemblyBytes.begin() +
                                      static_cast<std::ptrdiff_t>(std::size_t{fragment.fragIndex} * MAX_SHAPE_PAYLOAD));
        }
        if (assemblyFragmentsReceived == assemblyFragCount) {
            completeAssembly();
        }
        return true;
    }

    std::optional<std::uint32_t> CursorReceiver::takeShapeRequest(std::chrono::steady_clock::time_point now) noexcept {
        if (!missingShapeId || (lastRequestTime && now - *lastRequestTime < SHAPE_REQUEST_REPEAT_INTERVAL)) {
            return std::nullopt;
        }
        lastRequestTime = now;
        return missingShapeId;
    }

    void CursorReceiver::reset() noexcept {
        cache.clear();
        lastPositionSeq.reset();
        x = 0;
        y = 0;
        visible = false;
        shapeId = 0;
        drawnSlot.reset();
        missingShapeId.reset();
        lastRequestTime.reset();
        assembling = false;
        publish();
    }

    std::uint64_t CursorReceiver::snapshot() const noexcept {
        return published.load(std::memory_order_acquire);
    }

    std::optional<CursorShapeInfo> CursorReceiver::copyShape(std::uint64_t packedSnapshot,
                                                             std::span<std::uint32_t> argbPixels) const noexcept {
        const CursorSnapshot unpacked = CursorSnapshot::unpack(packedSnapshot);
        return cache.copy(unpacked.slot, unpacked.generation, argbPixels);
    }

    void CursorReceiver::expire(std::chrono::steady_clock::time_point now) noexcept {
        if (assembling && now - lastFragmentTime > FRAGMENT_TIMEOUT) {
            assembling = false;
        }
    }

    void CursorReceiver::startAssembly(const CursorShape& fragment) noexcept {
        assembling = true;
        assemblyShapeId = fragment.shapeId;
        assemblySeq = fragment.commonHeader.seq;
        assemblyFragCount = fragment.fragCount;
        assemblyFragmentsReceived = 0;
        assemblyInfo = infoOf(fragment);
        assemblyReceived.reset();
    }

    void CursorReceiver::completeAssembly() noexcept {
        assembling = false;
        const std::size_t totalBytes =
                std::size_t{assemblyInfo.width} * assemblyInfo.height * CursorShapeCache::BYTES_PER_PIXEL;
        const auto slot =
                cache.store(assemblyShapeId, assemblyInfo, std::span(assemblyBytes).first(totalBytes), drawnSlot);
        if (slot && missingShapeId == assemblyShapeId) {
            useSlot(*slot);
            publish();
        }
    }

    void CursorReceiver::useSlot(std::uint8_t slot) noexcept {
        drawnSlot = slot;
        cache.touch(slot);
        missingShapeId.reset();
    }

    void CursorReceiver::publish() noexcept {
        CursorSnapshot snapshot{.x = clampToInt16(x), .y = clampToInt16(y)};
        if (drawnSlot) {
            snapshot.visible = visible;
            snapshot.slot = *drawnSlot;
            snapshot.generation = cache.generation(*drawnSlot);
        }
        published.store(snapshot.pack(), std::memory_order_release);
    }
}
