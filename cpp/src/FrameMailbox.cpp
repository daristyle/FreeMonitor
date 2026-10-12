#include "FrameMailbox.hpp"

#include <cassert>
#include <utility>

namespace fm {
    void FrameMailbox::reserve(std::size_t frameBytes) {
        for (Slot& slot : slots) {
            if (slot.pixels.size() < frameBytes) {
                slot.pixels.resize(frameBytes);
            }
        }
    }

    void FrameMailbox::reset() {
        const std::lock_guard lock(mutex);
        writeIndex = 0;
        latestIndex = 1;
        readIndex = 2;
        hasLatest = false;
        reading = false;
        closed = false;
        droppedFrames = 0;
    }

    std::uint8_t* FrameMailbox::beginWrite(std::size_t frameBytes) {
        Slot& slot = slots[writeIndex];
        if (slot.pixels.size() < frameBytes) {
            slot.pixels.resize(frameBytes);
        }
        return slot.pixels.data();
    }

    void FrameMailbox::publish(const CapturedFrame& description) {
        Slot& written = slots[writeIndex];
        written.frame = description;
        written.frame.pixels = written.pixels.data();
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

    void FrameMailbox::close() noexcept {
        {
            const std::lock_guard lock(mutex);
            closed = true;
        }
        frameReady.notify_all();
    }

    CaptureStatus FrameMailbox::acquire(CapturedFrame& frame, std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex);
        assert(!reading && "release() must be called before the next acquire()");
        frameReady.wait_for(lock, timeout, [this] { return hasLatest || closed; });
        if (!hasLatest) {
            return closed ? CaptureStatus::CAPTURE_LOST : CaptureStatus::NO_NEW_FRAME;
        }
        std::swap(readIndex, latestIndex);
        hasLatest = false;
        reading = true;
        frame = slots[readIndex].frame;
        return CaptureStatus::FRAME_READY;
    }

    void FrameMailbox::release() noexcept {
        const std::lock_guard lock(mutex);
        reading = false;
    }

    std::uint64_t FrameMailbox::droppedFrameCount() noexcept {
        const std::lock_guard lock(mutex);
        return droppedFrames;
    }
}
