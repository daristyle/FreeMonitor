#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace fm {
    enum class MailboxStatus : std::uint8_t {
        FRAME_READY,
        NO_NEW_FRAME,
        CLOSED,
    };

    template<typename Description>
    class FrameMailbox {
    private:
        static constexpr std::size_t SLOT_COUNT = 3;

        struct Slot {
            std::vector<std::uint8_t> bytes;
            std::size_t size = 0;
            Description description{};
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
        void publish(const Description& description, std::size_t frameBytes);
        void close() noexcept;

        MailboxStatus acquire(Description& description, std::span<const std::uint8_t>& bytes,
                              std::chrono::milliseconds timeout) noexcept;
        void release() noexcept;

        std::uint64_t droppedFrameCount() noexcept;
    };
}

#include "FrameMailbox.tpp"
