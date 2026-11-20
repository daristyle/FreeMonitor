#include "UDPSocket.hpp"
#include "posix/NetworkInterfaces.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_CASE("the broadcast addresses of the IPv4 networks leave out loopback and duplicates") {
    const auto addresses = fm::ipv4BroadcastAddresses();
    REQUIRE(addresses);
    for (const std::uint32_t address : *addresses) {
        CHECK(address != 0);
        CHECK((address >> 24) != 127);
        CHECK(std::ranges::count(*addresses, address) == 1);
    }
}
