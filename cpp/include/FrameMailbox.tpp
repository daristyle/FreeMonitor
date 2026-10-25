#pragma once

#include <cassert>
#include <utility>

namespace fm {
    template<typename Description>
    void FrameMailbox<Description>::reserve(std::size_t frameBytes) {
        for (Slot& slot : slots) {
            if (slot.bytes.size() < frameBytes) {
                slot.bytes.resize(frameBytes);
            }
        }
    }

    template<typename Description>
    void FrameMailbox<Description>::reset() {
        const std::lock_guard lock(mutex);
        writeIndex = 0;
        latestIndex = 1;
        readIndex = 2;
        hasLatest = false;
        reading = false;
        closed = false;
        droppedFrames = 0;
    }

    template<typename Description>
    std::uint8_t* FrameMailbox<Description>::beginWrite(std::size_t frameBytes) {
        Slot& slot = slots[writeIndex];
        if (slot.bytes.size() < frameBytes) {
            slot.bytes.resize(frameBytes);
        }
        return slot.bytes.data();
    }

    template<typename Description>
    void FrameMailbox<Description>::publish(const Description& description, std::size_t frameBytes) {
        Slot& written = slots[writeIndex];
        assert(frameBytes <= written.bytes.size() && "publish() needs a matching beginWrite()");
        written.description = description;
        written.size = frameBytes;
        {
            const std::lock_guard lock(mutex);
            if (hasLatest) {
                ++droppedFrames;
            }
            std::swap(writeIndex, latestIndex);
            hasLatest = true;
        }
        frameReady.notify_one();
    }

    template<typename Description>
    void FrameMailbox<Description>::close() noexcept {
        {
            const std::lock_guard lock(mutex);
            closed = true;
        }
        frameReady.notify_all();
    }

    template<typename Description>
    MailboxStatus FrameMailbox<Description>::acquire(Description& description, std::span<const std::uint8_t>& bytes,
                                                     std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex);
        assert(!reading && "release() must be called before the next acquire()");
        frameReady.wait_for(lock, timeout, [this] { return hasLatest || closed; });
        if (!hasLatest) {
            return closed ? MailboxStatus::CLOSED : MailboxStatus::NO_NEW_FRAME;
        }
        std::swap(readIndex, latestIndex);
        hasLatest = false;
        reading = true;
        const Slot& slot = slots[readIndex];
        description = slot.description;
        bytes = std::span<const std::uint8_t>(slot.bytes).first(slot.size);
        return MailboxStatus::FRAME_READY;
    }

    template<typename Description>
    void FrameMailbox<Description>::release() noexcept {
        const std::lock_guard lock(mutex);
        reading = false;
    }

    template<typename Description>
    std::uint64_t FrameMailbox<Description>::droppedFrameCount() noexcept {
        const std::lock_guard lock(mutex);
        return droppedFrames;
    }
}
