#include "PeerTracker.hpp"

#include "SerialNumber.hpp"

namespace fm {
    void PeerTracker::reset(const Endpoint& address, std::chrono::steady_clock::time_point now) noexcept {
        peerAddress = address;
        highestSeq = 0;
        seqSeen = false;
        lastPacketTime = now;
        packetsReceived = 0;
        packetsLost = 0;
        bytesReceived = 0;
    }

    void PeerTracker::notePacket(const Endpoint& from, std::uint32_t seq, std::size_t bytes,
                                 std::chrono::steady_clock::time_point now) noexcept {
        lastPacketTime = now;
        ++packetsReceived;
        bytesReceived += static_cast<std::uint32_t>(bytes);
        if (!seqSeen) {
            seqSeen = true;
            highestSeq = seq;
            peerAddress = from;
            return;
        }
        const std::int32_t difference = serialDifference(seq, highestSeq);
        if (difference > 0) {
            packetsLost += static_cast<std::uint32_t>(difference - 1);
            highestSeq = seq;
            peerAddress = from;
        } else if (difference < 0 && packetsLost > 0) {
            --packetsLost;
        }
    }

    const Endpoint& PeerTracker::address() const noexcept {
        return peerAddress;
    }

    bool PeerTracker::silentFor(std::chrono::steady_clock::time_point now,
                                std::chrono::milliseconds timeout) const noexcept {
        return now - lastPacketTime >= timeout;
    }

    std::uint32_t PeerTracker::received() const noexcept {
        return packetsReceived;
    }

    std::uint32_t PeerTracker::lost() const noexcept {
        return packetsLost;
    }

    std::uint32_t PeerTracker::bytes() const noexcept {
        return bytesReceived;
    }
}
