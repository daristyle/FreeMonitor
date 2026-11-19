#include "CursorPackets.hpp"

#include "Packets.hpp"

#include <algorithm>

namespace fm {
    namespace {
        constexpr std::size_t BYTES_PER_PIXEL = 4;

        bool followsShapeRules(const CursorImage& image) noexcept {
            return image.width >= 1 && image.height >= 1 && image.width <= MAX_CURSOR_IMAGE_SIDE &&
                   image.height <= MAX_CURSOR_IMAGE_SIDE && image.hotspotX < image.width &&
                   image.hotspotY < image.height &&
                   image.pixels.size() == std::size_t{image.width} * image.height * BYTES_PER_PIXEL;
        }
    }

    std::size_t writeCursorPos(const CursorState& state, std::uint32_t sessionId, std::uint32_t seq,
                               std::uint64_t timestampUs, std::span<std::uint8_t> buffer) noexcept {
        CursorPos packet;
        packet.commonHeader.sessionId = sessionId;
        packet.commonHeader.seq = seq;
        packet.commonHeader.timestampUs = timestampUs;
        packet.shapeId = state.imageId;
        packet.x = state.x;
        packet.y = state.y;
        packet.flags = state.visible ? CursorFlags::VISIBLE : std::uint8_t{0};
        return packet.write(buffer);
    }

    std::uint16_t cursorShapeFragmentCount(const CursorImage& image) noexcept {
        if (!followsShapeRules(image)) {
            return 0;
        }
        return static_cast<std::uint16_t>((image.pixels.size() + MAX_SHAPE_PAYLOAD - 1) / MAX_SHAPE_PAYLOAD);
    }

    std::size_t writeCursorShapeFragment(const CursorImage& image, std::uint32_t shapeId, std::uint16_t fragIndex,
                                         std::uint32_t sessionId, std::uint32_t seq, std::uint64_t timestampUs,
                                         std::span<std::uint8_t> buffer) noexcept {
        const std::uint16_t fragCount = cursorShapeFragmentCount(image);
        if (fragIndex >= fragCount) {
            return 0;
        }
        const std::size_t offset = std::size_t{fragIndex} * MAX_SHAPE_PAYLOAD;
        CursorShape packet;
        packet.commonHeader.sessionId = sessionId;
        packet.commonHeader.seq = seq;
        packet.commonHeader.timestampUs = timestampUs;
        packet.shapeId = shapeId;
        packet.fragIndex = fragIndex;
        packet.fragCount = fragCount;
        packet.width = image.width;
        packet.height = image.height;
        packet.hotspotX = image.hotspotX;
        packet.hotspotY = image.hotspotY;
        packet.payload =
                std::span(image.pixels)
                        .subspan(offset, std::min<std::size_t>(MAX_SHAPE_PAYLOAD, image.pixels.size() - offset));
        return packet.write(buffer);
    }
}
