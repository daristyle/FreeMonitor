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

        bool isValidFragment(const VideoFragment& fragment) noexcept {
            if (fragment.fragCount == 0 || fragment.fragIndex >= fragment.fragCount) {
                return false;
            }
            if (fragment.frameSize == 0 || fragment.frameSize > MAX_FRAME_SIZE) {
                return false;
            }
            if (fragment.fragCount != (fragment.frameSize + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD) {
                return false;
            }
            const std::size_t offset = payloadOffset(fragment.fragIndex);
            if (fragment.payload.size() != std::min<std::size_t>(MAX_FRAGMENT_PAYLOAD, fragment.frameSize - offset)) {
                return false;
            }
            if (!toCodec(fragment.codec).has_value()) {
                return false;
            }
            return (fragment.flags & ~FragmentFlags::KEYFRAME) == 0 && fragment.reserved == 0;
        }
    }

    Reassembler::Reassembler() {
        for (Slot& slot : slots) {
            slot.data.resize(MAX_FRAME_SIZE);
        }
    }

    bool Reassembler::matches(const Slot& slot, const VideoFragment& fragment) noexcept {
        return slot.fragCount == fragment.fragCount && slot.frameSize == fragment.frameSize &&
               toCodec(fragment.codec) == slot.codec && slot.flags == fragment.flags &&
               slot.captureTimestampUs == fragment.captureTimestampUs;
    }

    void Reassembler::start(Slot& slot, const VideoFragment& fragment,
                            std::chrono::steady_clock::time_point now) noexcept {
        slot.received.reset();
        slot.firstFragmentTime = now;
        slot.captureTimestampUs = fragment.captureTimestampUs;
        slot.frameId = fragment.frameId;
        slot.frameSize = fragment.frameSize;
        slot.fragCount = fragment.fragCount;
        slot.receivedCount = 0;
        slot.codec = *toCodec(fragment.codec);
        slot.flags = fragment.flags;
        slot.inUse = true;
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

    void Reassembler::discard(Slot& slot) noexcept {
        slot.inUse = false;
        ++droppedCount;
    }

    void Reassembler::deliver(Slot& slot, std::chrono::steady_clock::time_point now) noexcept {
        completed.data = std::span<const std::uint8_t>(slot.data).first(slot.frameSize);
        completed.frameId = slot.frameId;
        completed.codec = slot.codec;
        completed.keyframe = (slot.flags & FragmentFlags::KEYFRAME) != 0;
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
        if (const Slot* existing = findSlot(fragment.frameId); existing != nullptr && !matches(*existing, fragment)) {
            return FragmentStatus::INVALID;
        }
        if (lastDeliveredFrameId.has_value() && !isNewer(fragment.frameId, *lastDeliveredFrameId)) {
            return FragmentStatus::TOO_OLD;
        }
        expire(now);
        Slot* slot = findSlot(fragment.frameId);
        if (slot == nullptr) {
            slot = claimSlot(fragment.frameId);
            if (slot == nullptr) {
                return FragmentStatus::TOO_OLD;
            }
            start(*slot, fragment, now);
        }
        if (slot->received.test(fragment.fragIndex)) {
            return FragmentStatus::DUPLICATE;
        }
        std::ranges::copy(fragment.payload, slot->data.data() + payloadOffset(fragment.fragIndex));
        slot->received.set(fragment.fragIndex);
        ++slot->receivedCount;
        slot->lastProgress = now;
        if (slot->receivedCount < slot->fragCount) {
            return FragmentStatus::ACCEPTED;
        }
        deliver(*slot, now);
        return FragmentStatus::FRAME_COMPLETE;
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
