#include "Reassembler.hpp"

#include "SerialNumber.hpp"

#include <algorithm>
#include <cstddef>

namespace fm {
    namespace {
        constexpr std::chrono::milliseconds FRAGMENT_TIMEOUT{FRAGMENT_TIMEOUT_MS};

        std::size_t payloadOffset(std::uint16_t fragIndex) noexcept {
            return std::size_t{fragIndex} * MAX_FRAGMENT_PAYLOAD;
        }

        template<typename Packet>
        bool hasValidFrameDetails(const Packet& packet) noexcept {
            if (packet.fragCount == 0 || packet.frameSize == 0 || packet.frameSize > MAX_FRAME_SIZE) {
                return false;
            }
            if (packet.fragCount != (packet.frameSize + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD) {
                return false;
            }
            return toCodec(packet.codec).has_value() && (packet.flags & ~FragmentFlags::KEYFRAME) == 0;
        }

        bool isValidFragment(const VideoFragment& fragment) noexcept {
            if (!hasValidFrameDetails(fragment) || fragment.fragIndex >= fragment.fragCount || fragment.reserved != 0) {
                return false;
            }
            const std::size_t offset = payloadOffset(fragment.fragIndex);
            return fragment.payload.size() == std::min<std::size_t>(MAX_FRAGMENT_PAYLOAD, fragment.frameSize - offset);
        }

        bool isValidParity(const VideoParity& parity) noexcept {
            if (!hasValidFrameDetails(parity) || parity.parityIndex >= parity.parityCount) {
                return false;
            }
            const std::uint32_t mostParity = std::uint32_t{fecGroupCount(parity.fragCount)} * MAX_FEC_GROUP_PARITY;
            return parity.parityCount <= mostParity && parity.payload.size() == fecBlockSize(parity.frameSize);
        }
    }

    Reassembler::Reassembler() {
        for (Slot& slot : slots) {
            slot.data.resize(MAX_FRAME_SIZE);
            slot.parity.resize(std::size_t{MAX_PARITY_COUNT} * MAX_PARITY_PAYLOAD);
        }
    }

    Reassembler::FrameDetails Reassembler::detailsOf(const VideoFragment& fragment) noexcept {
        return FrameDetails{.captureTimestampUs = fragment.captureTimestampUs,
                            .frameId = fragment.frameId,
                            .frameSize = fragment.frameSize,
                            .fragCount = fragment.fragCount,
                            .codec = fragment.codec,
                            .flags = fragment.flags};
    }

    Reassembler::FrameDetails Reassembler::detailsOf(const VideoParity& parity) noexcept {
        return FrameDetails{.captureTimestampUs = parity.captureTimestampUs,
                            .frameId = parity.frameId,
                            .frameSize = parity.frameSize,
                            .fragCount = parity.fragCount,
                            .codec = parity.codec,
                            .flags = parity.flags};
    }

    bool Reassembler::matches(const Slot& slot, const FrameDetails& details) noexcept {
        return slot.fragCount == details.fragCount && slot.frameSize == details.frameSize &&
               toCodec(details.codec) == slot.codec && slot.flags == details.flags &&
               slot.captureTimestampUs == details.captureTimestampUs;
    }

    void Reassembler::start(Slot& slot, const FrameDetails& details,
                            std::chrono::steady_clock::time_point now) noexcept {
        slot.received.reset();
        slot.parityReceived.reset();
        slot.firstFragmentTime = now;
        slot.captureTimestampUs = details.captureTimestampUs;
        slot.frameId = details.frameId;
        slot.frameSize = details.frameSize;
        slot.fragCount = details.fragCount;
        slot.receivedCount = 0;
        slot.parityCount = 0;
        slot.codec = *toCodec(details.codec);
        slot.flags = details.flags;
        slot.repaired = false;
        slot.inUse = true;
    }

    void Reassembler::repairGroup(Slot& slot, std::size_t group) noexcept {
        if (slot.parityCount == 0) {
            return;
        }
        const std::size_t groupCount = fecGroupCount(slot.fragCount);
        std::array<std::uint16_t, MAX_FEC_GROUP_PARITY> missing{};
        std::size_t missingCount = 0;
        for (std::size_t fragIndex = group; fragIndex < slot.fragCount; fragIndex += groupCount) {
            if (slot.received.test(fragIndex)) {
                continue;
            }
            if (missingCount == missing.size()) {
                return;
            }
            missing[missingCount] = static_cast<std::uint16_t>(fragIndex);
            ++missingCount;
        }
        if (missingCount == 0) {
            return;
        }

        const std::size_t blockSize = fecBlockSize(slot.frameSize);
        std::array<ParityBlock, MAX_FEC_GROUP_PARITY> blocks{};
        std::size_t blockCount = 0;
        for (std::size_t parityIndex = group; parityIndex < slot.parityCount && blockCount < missingCount;
             parityIndex += groupCount) {
            if (slot.parityReceived.test(parityIndex)) {
                blocks[blockCount] =
                        ParityBlock{static_cast<std::uint16_t>(parityIndex),
                                    std::span(slot.parity).subspan(parityIndex * MAX_PARITY_PAYLOAD, blockSize)};
                ++blockCount;
            }
        }
        if (blockCount < missingCount ||
            !recoverFragments(std::span(slot.data).first(slot.frameSize), std::span(missing).first(missingCount),
                              std::span<const ParityBlock>(blocks).first(blockCount))) {
            return;
        }
        for (std::size_t index = 0; index < missingCount; ++index) {
            slot.received.set(missing[index]);
        }
        slot.receivedCount = static_cast<std::uint16_t>(slot.receivedCount + missingCount);
        slot.repaired = true;
    }

    Reassembler::Slot* Reassembler::findSlot(std::uint32_t frameId) noexcept {
        for (Slot& slot : slots) {
            if (slot.inUse && slot.frameId == frameId) {
                return &slot;
            }
        }
        return nullptr;
    }

    Reassembler::Slot* Reassembler::claimSlot(std::uint32_t frameId) noexcept {
        Slot* oldest = nullptr;
        for (Slot& slot : slots) {
            if (!slot.inUse) {
                return &slot;
            }
            if (oldest == nullptr || isNewer(oldest->frameId, slot.frameId)) {
                oldest = &slot;
            }
        }
        if (!isNewer(frameId, oldest->frameId)) {
            return nullptr;
        }
        discard(*oldest);
        return oldest;
    }

    Reassembler::Slot* Reassembler::slotFor(const FrameDetails& details, std::chrono::steady_clock::time_point now,
                                            FragmentStatus& refusal) noexcept {
        Slot* slot = findSlot(details.frameId);
        if (slot != nullptr && !matches(*slot, details)) {
            refusal = FragmentStatus::INVALID;
            return nullptr;
        }
        if (lastDeliveredFrameId.has_value() && !isNewer(details.frameId, *lastDeliveredFrameId)) {
            refusal = FragmentStatus::TOO_OLD;
            return nullptr;
        }
        if (slot == nullptr) {
            slot = claimSlot(details.frameId);
            if (slot == nullptr) {
                refusal = FragmentStatus::TOO_OLD;
                return nullptr;
            }
            start(*slot, details, now);
        }
        return slot;
    }

    FragmentStatus Reassembler::packetStored(Slot& slot, std::size_t group,
                                             std::chrono::steady_clock::time_point now) noexcept {
        slot.lastProgress = now;
        repairGroup(slot, group);
        if (slot.receivedCount < slot.fragCount) {
            return FragmentStatus::ACCEPTED;
        }
        deliver(slot, now);
        return FragmentStatus::FRAME_COMPLETE;
    }

    void Reassembler::discard(Slot& slot) noexcept {
        slot.inUse = false;
        ++droppedCount;
    }

    void Reassembler::deliver(Slot& slot, std::chrono::steady_clock::time_point now) noexcept {
        completed.data = std::span<const std::uint8_t>(slot.data).first(slot.frameSize);
        completed.frameId = slot.frameId;
        completed.codec = slot.codec;
        completed.keyframe = (slot.flags & FragmentFlags::KEYFRAME) != 0;
        completed.repaired = slot.repaired;
        completed.captureTimestampUs = slot.captureTimestampUs;
        completed.firstFragmentTime = slot.firstFragmentTime;
        completed.completeTime = now;
        lastDeliveredFrameId = slot.frameId;
        slot.inUse = false;
        ++completedCount;
        for (Slot& other : slots) {
            if (other.inUse && isNewer(slot.frameId, other.frameId)) {
                discard(other);
            }
        }
    }

    FragmentStatus Reassembler::accept(const VideoFragment& fragment,
                                       std::chrono::steady_clock::time_point now) noexcept {
        if (!isValidFragment(fragment)) {
            return FragmentStatus::INVALID;
        }
        expire(now);
        FragmentStatus refusal = FragmentStatus::INVALID;
        Slot* slot = slotFor(detailsOf(fragment), now, refusal);
        if (slot == nullptr) {
            return refusal;
        }
        if (slot->received.test(fragment.fragIndex)) {
            return FragmentStatus::DUPLICATE;
        }
        std::ranges::copy(fragment.payload, slot->data.data() + payloadOffset(fragment.fragIndex));
        slot->received.set(fragment.fragIndex);
        ++slot->receivedCount;
        return packetStored(*slot, fragment.fragIndex % fecGroupCount(fragment.fragCount), now);
    }

    FragmentStatus Reassembler::accept(const VideoParity& parity, std::chrono::steady_clock::time_point now) noexcept {
        if (!isValidParity(parity)) {
            return FragmentStatus::INVALID;
        }
        expire(now);
        FragmentStatus refusal = FragmentStatus::INVALID;
        Slot* slot = slotFor(detailsOf(parity), now, refusal);
        if (slot == nullptr) {
            return refusal;
        }
        if (slot->parityCount != 0 && slot->parityCount != parity.parityCount) {
            return FragmentStatus::INVALID;
        }
        if (slot->parityReceived.test(parity.parityIndex)) {
            return FragmentStatus::DUPLICATE;
        }
        std::ranges::copy(parity.payload, slot->parity.data() + std::size_t{parity.parityIndex} * MAX_PARITY_PAYLOAD);
        slot->parityReceived.set(parity.parityIndex);
        slot->parityCount = parity.parityCount;
        return packetStored(*slot, parity.parityIndex % fecGroupCount(parity.fragCount), now);
    }

    void Reassembler::expire(std::chrono::steady_clock::time_point now) noexcept {
        for (Slot& slot : slots) {
            if (slot.inUse && now - slot.lastProgress >= FRAGMENT_TIMEOUT) {
                discard(slot);
            }
        }
    }

    void Reassembler::reset() noexcept {
        for (Slot& slot : slots) {
            slot.inUse = false;
        }
        completed = {};
        lastDeliveredFrameId.reset();
        completedCount = 0;
        droppedCount = 0;
    }

    ReassembledFrame Reassembler::completedFrame() const noexcept {
        return completed;
    }

    std::uint32_t Reassembler::framesCompleted() const noexcept {
        return completedCount;
    }

    std::uint32_t Reassembler::framesDropped() const noexcept {
        return droppedCount;
    }
}
