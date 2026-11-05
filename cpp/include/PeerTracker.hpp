#pragma once

#include "UDPSocket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace fm {
    class PeerTracker {
    private:
        Endpoint peerAddress{};
        std::uint32_t highestSeq = 0;
        bool seqSeen = false;
        std::chrono::steady_clock::time_point lastPacketTime{};
        std::uint32_t packetsReceived = 0;
        std::uint32_t packetsLost = 0;
        std::uint32_t bytesReceived = 0;

    public:
        void reset(const Endpoint& address, std::chrono::steady_clock::time_point now) noexcept;
        void notePacket(const Endpoint& from, std::uint32_t seq, std::size_t bytes,
                        std::chrono::steady_clock::time_point now) noexcept;

        const Endpoint& address() const noexcept;
        bool silentFor(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const noexcept;
        std::uint32_t received() const noexcept;
        std::uint32_t lost() const noexcept;
        std::uint32_t bytes() const noexcept;
    };
}
