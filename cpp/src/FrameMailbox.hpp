#pragma once

#include "CaptureSource.hpp"
#include "Frame.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace fm {
    class FrameMailbox {
    private:
        static constexpr std::size_t SLOT_COUNT = 3;

        struct Slot {
            std::vector<std::uint8_t> pixels;
            CapturedFrame frame;
        };

        std::array<Slot, SLOT_COUNT> slots;
        std::mutex mutex;
        std::condition_variable frameReady;
        std::size_t writeIndex = 0;
        std::size_t latestIndex = 1;
        std::size_t readIndex = 2;
        bool hasLatest = false;
        bool reading = false;
        bool closed = false;
        std::uint64_t droppedFrames = 0;

    public:
        // Setup only: call before the stream starts, while no frame is being written or read.
        void reserve(std::size_t frameBytes);
        void reset();

        std::uint8_t* beginWrite(std::size_t frameBytes);
        void publish(const CapturedFrame& description);
        void close() noexcept;

        CaptureStatus acquire(CapturedFrame& frame, std::chrono::milliseconds timeout) noexcept;
        void release() noexcept;

        std::uint64_t droppedFrameCount() noexcept;
    };
}
