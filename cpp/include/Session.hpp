#pragma once

#include "UDPSocket.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace fm {
    class SequenceCounter {
    private:
        std::shared_ptr<std::atomic<std::uint32_t>> counter;

    public:
        SequenceCounter() : SequenceCounter(0) {}

        explicit SequenceCounter(std::uint32_t first) : counter(std::make_shared<std::atomic<std::uint32_t>>(first)) {}

        std::uint32_t next() noexcept {
            return counter->fetch_add(1, std::memory_order_relaxed);
        }

        std::uint32_t peek() const noexcept {
            return counter->load(std::memory_order_relaxed);
        }
    };

    struct Session {
        Endpoint peer;
        std::uint32_t sessionId = 0;
        SequenceCounter seq;
        std::uint8_t features = 0;
    };
}
