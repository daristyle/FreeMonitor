#include "Fragmenter.hpp"

#include "ReedSolomon.hpp"

#include <algorithm>

namespace fm {
    bool Fragmenter::begin(std::span<const std::uint8_t> frameData, std::uint32_t frameId, Codec codec, bool keyframe,
                           std::uint64_t captureTimestampUs) noexcept {
        if (frameData.empty() || frameData.size() > MAX_FRAME_SIZE) {
            this->frameData = {};
            fragment.fragCount = 0;
            return false;
        }
        this->frameData = frameData;
        fragment.frameId = frameId;
        fragment.fragCount =
                static_cast<std::uint16_t>((frameData.size() + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);
        fragment.frameSize = static_cast<std::uint32_t>(frameData.size());
        fragment.codec = static_cast<std::uint8_t>(codec);
        fragment.flags = keyframe ? FragmentFlags::KEYFRAME : std::uint8_t{0};
        fragment.captureTimestampUs = captureTimestampUs;
        return true;
    }

    std::uint16_t Fragmenter::fragmentCount() const noexcept {
        return fragment.fragCount;
    }

    std::size_t Fragmenter::write(std::uint16_t fragIndex, std::uint32_t sessionId, std::uint32_t seq,
                                  std::uint64_t timestampUs, std::span<std::uint8_t> packetBuffer) const noexcept {
        if (fragIndex >= fragment.fragCount) {
            return 0;
        }
        const std::size_t offset = std::size_t{fragIndex} * MAX_FRAGMENT_PAYLOAD;
        VideoFragment packet = fragment;
        packet.commonHeader.sessionId = sessionId;
        packet.commonHeader.seq = seq;
        packet.commonHeader.timestampUs = timestampUs;
        packet.fragIndex = fragIndex;
        packet.payload =
                frameData.subspan(offset, std::min<std::size_t>(MAX_FRAGMENT_PAYLOAD, frameData.size() - offset));
        return packet.write(packetBuffer);
    }

    std::size_t Fragmenter::writeParity(std::uint16_t parityIndex, std::uint16_t parityCount, std::uint32_t sessionId,
                                        std::uint32_t seq, std::uint64_t timestampUs,
                                        std::span<std::uint8_t> packetBuffer) const noexcept {
        const std::size_t blockSize = fecBlockSize(fragment.frameSize);
        if (parityIndex >= parityCount || packetBuffer.size() < VideoParity::SIZE + blockSize) {
            return 0;
        }
        VideoParity packet;
        packet.commonHeader.sessionId = sessionId;
        packet.commonHeader.seq = seq;
        packet.commonHeader.timestampUs = timestampUs;
        packet.frameId = fragment.frameId;
        packet.parityIndex = parityIndex;
        packet.fragCount = fragment.fragCount;
        packet.frameSize = fragment.frameSize;
        packet.codec = fragment.codec;
        packet.flags = fragment.flags;
        packet.parityCount = parityCount;
        packet.captureTimestampUs = fragment.captureTimestampUs;
        if (packet.write(packetBuffer) != VideoParity::SIZE ||
            !computeParity(frameData, parityIndex, packetBuffer.subspan(VideoParity::SIZE, blockSize))) {
            return 0;
        }
        return VideoParity::SIZE + blockSize;
    }
}
