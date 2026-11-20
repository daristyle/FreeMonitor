#pragma once

#include "Error.hpp"
#include "Packets.hpp"
#include "UDPSocket.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace fm {
    constexpr std::uint32_t LIMITED_BROADCAST_IP_ADDRESS = makeIPAddress(255, 255, 255, 255);

    struct DiscoveredHost {
        Endpoint endpoint;
        std::string name;
        std::uint8_t version = 0;

        friend bool operator==(const DiscoveredHost&, const DiscoveredHost&) = default;
    };

    struct DiscoverySearch {
        std::vector<Endpoint> targets;
        std::uint32_t nonce = 0;
        std::uint32_t firstSeq = 0;
        std::chrono::milliseconds duration{DISCOVERY_TIME_MS};
        std::chrono::milliseconds repeatInterval{DISCOVER_REPEAT_INTERVAL_MS};
    };

    Result<std::vector<DiscoveredHost>> discoverHosts(const DiscoverySearch& search);
}
