#pragma once

#include "Error.hpp"
#include "Packets.hpp"
#include "RateLimiter.hpp"
#include "UDPSocket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace fm {
    class DiscoveryResponder {
    private:
        using Clock = std::chrono::steady_clock;

        static constexpr std::chrono::milliseconds RECEIVE_POLL_INTERVAL{100};
        static constexpr std::chrono::milliseconds RATE_WINDOW{1000};

        UDPSocket udpSocket;
        std::string name;
        std::uint16_t sessionPort;
        std::uint32_t nextSeq;
        RateLimiter limiter;
        std::array<std::uint8_t, MAX_PACKET_SIZE> receiveBuffer{};
        std::array<std::uint8_t, MAX_PACKET_SIZE> sendBuffer{};
        std::atomic<bool> stopRequested{false};
        std::atomic<std::uint64_t> announcesSent{0};
        std::atomic<std::uint64_t> discoversLimited{0};
        std::thread thread;

        DiscoveryResponder(UDPSocket udpSocket, std::string_view name, std::uint16_t sessionPort,
                           std::uint32_t firstSeq);

        void run() noexcept;
        void answer(std::span<const std::uint8_t> packet, const Endpoint& from, Clock::time_point now) noexcept;

    public:
        static Result<std::unique_ptr<DiscoveryResponder>> open(const Endpoint& bindTo, std::string_view name,
                                                                std::uint16_t sessionPort, std::uint32_t firstSeq);
        ~DiscoveryResponder();

        DiscoveryResponder(const DiscoveryResponder&) = delete;
        DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

        void start();
        void stop() noexcept;
        Result<Endpoint> localEndpoint() const;
        const std::string& hostName() const noexcept;
        std::uint64_t announceCount() const noexcept;
        std::uint64_t limitedCount() const noexcept;
    };
}
