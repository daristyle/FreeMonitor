#include "DiscoveryResponder.hpp"

#include "DiscoveryPackets.hpp"
#include "Timestamp.hpp"

namespace fm {
    DiscoveryResponder::DiscoveryResponder(UDPSocket udpSocket, std::string_view name, std::uint16_t sessionPort,
                                           std::uint32_t firstSeq)
        : udpSocket(std::move(udpSocket)), name(fitHostName(name)), sessionPort(sessionPort), nextSeq(firstSeq),
          limiter(MAX_ANNOUNCES_PER_SECOND, RATE_WINDOW) {}

    Result<std::unique_ptr<DiscoveryResponder>> DiscoveryResponder::open(const Endpoint& bindTo, std::string_view name,
                                                                         std::uint16_t sessionPort,
                                                                         std::uint32_t firstSeq) {
        if (sessionPort == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "the session port must not be 0"});
        }
        auto udpSocket = UDPSocket::open(bindTo);
        if (!udpSocket) {
            return std::unexpected(udpSocket.error());
        }
        return std::unique_ptr<DiscoveryResponder>(
                new DiscoveryResponder(std::move(*udpSocket), name, sessionPort, firstSeq));
    }

    DiscoveryResponder::~DiscoveryResponder() {
        stop();
    }

    void DiscoveryResponder::start() {
        if (thread.joinable()) {
            return;
        }
        stopRequested.store(false, std::memory_order_relaxed);
        thread = std::thread([this] { run(); });
    }

    void DiscoveryResponder::stop() noexcept {
        if (!thread.joinable()) {
            return;
        }
        stopRequested.store(true, std::memory_order_relaxed);
        thread.join();
    }

    Result<Endpoint> DiscoveryResponder::localEndpoint() const {
        return udpSocket.localEndpoint();
    }

    const std::string& DiscoveryResponder::hostName() const noexcept {
        return name;
    }

    std::uint64_t DiscoveryResponder::announceCount() const noexcept {
        return announcesSent.load(std::memory_order_relaxed);
    }

    std::uint64_t DiscoveryResponder::limitedCount() const noexcept {
        return discoversLimited.load(std::memory_order_relaxed);
    }

    void DiscoveryResponder::run() noexcept {
        Endpoint from;
        while (!stopRequested.load(std::memory_order_relaxed)) {
            const ReceiveResult result = udpSocket.receiveFrom(receiveBuffer, from, RECEIVE_POLL_INTERVAL);
            if (result.status == ReceiveStatus::RECEIVED) {
                answer(std::span<const std::uint8_t>(receiveBuffer).first(result.size), from, Clock::now());
            } else if (result.status == ReceiveStatus::RECEIVE_FAILED) {
                std::this_thread::sleep_for(RECEIVE_POLL_INTERVAL);
            }
        }
    }

    void DiscoveryResponder::answer(std::span<const std::uint8_t> packet, const Endpoint& from,
                                    Clock::time_point now) noexcept {
        const auto nonce = readDiscover(packet);
        if (!nonce) {
            return;
        }
        if (!limiter.allow(now)) {
            discoversLimited.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::size_t size = writeAnnounce(*nonce, sessionPort, name, nextSeq++, toTimestampUs(now), sendBuffer);
        if (size > 0 && udpSocket.sendTo(std::span(sendBuffer).first(size), from) == SendStatus::SENT) {
            announcesSent.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
