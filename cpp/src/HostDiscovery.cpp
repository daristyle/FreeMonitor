#include "HostDiscovery.hpp"

#include "DiscoveryPackets.hpp"
#include "Timestamp.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace fm {
    Result<std::vector<DiscoveredHost>> discoverHosts(const DiscoverySearch& search) {
        using Clock = std::chrono::steady_clock;

        if (search.targets.empty() || search.repeatInterval <= std::chrono::milliseconds::zero()) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "a search needs targets and a repeat interval"});
        }
        auto udpSocket = UDPSocket::open(Endpoint{ANY_IP_ADDRESS, 0});
        if (!udpSocket) {
            return std::unexpected(udpSocket.error());
        }
        if (auto broadcast = udpSocket->enableBroadcast(); !broadcast) {
            return std::unexpected(broadcast.error());
        }

        std::array<std::uint8_t, Discover::SIZE> discoverBuffer{};
        std::array<std::uint8_t, MAX_PACKET_SIZE> receiveBuffer{};
        std::vector<DiscoveredHost> hosts;
        std::uint32_t seq = search.firstSeq;
        bool anySent = false;
        const Clock::time_point end = Clock::now() + search.duration;
        Clock::time_point nextSend = Clock::now();

        for (Clock::time_point now = Clock::now(); now < end; now = Clock::now()) {
            if (now >= nextSend) {
                const std::size_t size = writeDiscover(search.nonce, seq++, toTimestampUs(now), discoverBuffer);
                for (const Endpoint& target : search.targets) {
                    anySent = udpSocket->sendTo(std::span(discoverBuffer).first(size), target) == SendStatus::SENT ||
                              anySent;
                }
                nextSend = now + search.repeatInterval;
            }
            const auto wait = std::chrono::ceil<std::chrono::milliseconds>(std::min(nextSend, end) - now);
            Endpoint from;
            const ReceiveResult result = udpSocket->receiveFrom(receiveBuffer, from, wait);
            if (result.status == ReceiveStatus::RECEIVE_FAILED) {
                return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "receiving the answers to DISCOVER failed"});
            }
            if (result.status != ReceiveStatus::RECEIVED) {
                continue;
            }
            const auto announce =
                    readAnnounce(std::span<const std::uint8_t>(receiveBuffer).first(result.size), search.nonce);
            if (!announce) {
                continue;
            }
            const Endpoint endpoint{from.address, announce->port};
            if (std::ranges::none_of(hosts, [&](const DiscoveredHost& host) { return host.endpoint == endpoint; })) {
                hosts.push_back(DiscoveredHost{
                        .endpoint = endpoint, .name = std::string(announce->name), .version = announce->version});
            }
        }
        if (!anySent) {
            return std::unexpected(
                    Error{ErrorCode::DEVICE_FAILURE, "DISCOVER could not be sent to any address; is there a network?"});
        }
        return hosts;
    }
}
