#include "DiscoveryPackets.hpp"
#include "DiscoveryResponder.hpp"
#include "HostDiscovery.hpp"
#include "UDPSocket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::uint16_t SESSION_PORT = 50500;
    constexpr std::uint32_t NONCE = 0x5EA2C401;
    const fm::Endpoint ANY_LOOPBACK_PORT{fm::LOOPBACK_IP_ADDRESS, 0};

    std::unique_ptr<fm::DiscoveryResponder> startResponder(std::string_view name) {
        auto responder = fm::DiscoveryResponder::open(ANY_LOOPBACK_PORT, name, SESSION_PORT, 900);
        REQUIRE(responder);
        (*responder)->start();
        return std::move(*responder);
    }

    fm::Endpoint endpointOf(const fm::DiscoveryResponder& responder) {
        const auto endpoint = responder.localEndpoint();
        REQUIRE(endpoint);
        return *endpoint;
    }

    fm::DiscoverySearch searchFor(std::vector<fm::Endpoint> targets) {
        return fm::DiscoverySearch{.targets = std::move(targets),
                                   .nonce = NONCE,
                                   .firstSeq = 1,
                                   .duration = 300ms,
                                   .repeatInterval = 100ms};
    }
}

TEST_CASE("a search finds a responding host once, with its name, session port and version") {
    const auto responder = startResponder("fedora-laptop");
    const auto hosts = fm::discoverHosts(searchFor({endpointOf(*responder)}));
    REQUIRE(hosts);
    REQUIRE(hosts->size() == 1);
    CHECK(hosts->front() == fm::DiscoveredHost{.endpoint = {fm::LOOPBACK_IP_ADDRESS, SESSION_PORT},
                                               .name = "fedora-laptop",
                                               .version = fm::PROTOCOL_VERSION});
    CHECK(responder->announceCount() >= 2);
}

TEST_CASE("a search finds every host that answers") {
    const auto first = startResponder("first");
    const auto second = fm::DiscoveryResponder::open(ANY_LOOPBACK_PORT, "second", SESSION_PORT + 1, 1);
    REQUIRE(second);
    (*second)->start();
    const auto hosts = fm::discoverHosts(searchFor({endpointOf(*first), endpointOf(**second)}));
    REQUIRE(hosts);
    REQUIRE(hosts->size() == 2);
    CHECK(hosts->at(0).name != hosts->at(1).name);
}

TEST_CASE("a search with no host answering finds nothing") {
    auto silent = fm::UDPSocket::open(ANY_LOOPBACK_PORT);
    REQUIRE(silent);
    const auto endpoint = silent->localEndpoint();
    REQUIRE(endpoint);
    const auto hosts = fm::discoverHosts(searchFor({*endpoint}));
    REQUIRE(hosts);
    CHECK(hosts->empty());
}

TEST_CASE("a search ignores answers that belong to another search") {
    auto fakeHost = fm::UDPSocket::open(ANY_LOOPBACK_PORT);
    REQUIRE(fakeHost);
    const auto fakeEndpoint = fakeHost->localEndpoint();
    REQUIRE(fakeEndpoint);
    std::jthread answerer([&fakeHost] {
        std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
        fm::Endpoint from;
        for (int i = 0; i < 3; ++i) {
            if (fakeHost->receiveFrom(buffer, from, 300ms).status != fm::ReceiveStatus::RECEIVED) {
                return;
            }
            const std::size_t size = fm::writeAnnounce(NONCE + 1, SESSION_PORT, "stranger", 1, 1, buffer);
            fakeHost->sendTo(std::span(buffer).first(size), from);
        }
    });
    const auto hosts = fm::discoverHosts(searchFor({*fakeEndpoint}));
    REQUIRE(hosts);
    CHECK(hosts->empty());
}

TEST_CASE("the responder ignores packets that are not a valid DISCOVER") {
    const auto responder = startResponder("host");
    auto client = fm::UDPSocket::open(ANY_LOOPBACK_PORT);
    REQUIRE(client);
    std::array<std::uint8_t, fm::Discover::SIZE> discover{};
    REQUIRE(fm::writeDiscover(NONCE, 1, 1, discover) == fm::Discover::SIZE);
    discover[7] = 1;
    REQUIRE(client->sendTo(discover, endpointOf(*responder)) == fm::SendStatus::SENT);

    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    fm::Endpoint from;
    CHECK(client->receiveFrom(buffer, from, 200ms).status == fm::ReceiveStatus::NO_DATAGRAM);
    CHECK(responder->announceCount() == 0);
}

TEST_CASE("the responder answers at most MAX_ANNOUNCES_PER_SECOND times a second") {
    const auto responder = startResponder("host");
    auto client = fm::UDPSocket::open(ANY_LOOPBACK_PORT);
    REQUIRE(client);
    std::array<std::uint8_t, fm::Discover::SIZE> discover{};
    REQUIRE(fm::writeDiscover(NONCE, 1, 1, discover) == fm::Discover::SIZE);
    const std::uint32_t sent = fm::MAX_ANNOUNCES_PER_SECOND + 10;
    for (std::uint32_t i = 0; i < sent; ++i) {
        REQUIRE(client->sendTo(discover, endpointOf(*responder)) == fm::SendStatus::SENT);
    }

    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    fm::Endpoint from;
    std::uint32_t answers = 0;
    while (client->receiveFrom(buffer, from, 200ms).status == fm::ReceiveStatus::RECEIVED) {
        ++answers;
    }
    CHECK(answers == fm::MAX_ANNOUNCES_PER_SECOND);
    CHECK(responder->limitedCount() == sent - fm::MAX_ANNOUNCES_PER_SECOND);
}

TEST_CASE("a responder needs a session port and keeps a host name that fits the spec") {
    CHECK_FALSE(fm::DiscoveryResponder::open(ANY_LOOPBACK_PORT, "host", 0, 1));
    const auto unnamed = fm::DiscoveryResponder::open(ANY_LOOPBACK_PORT, "", SESSION_PORT, 1);
    REQUIRE(unnamed);
    CHECK((*unnamed)->hostName() == fm::DEFAULT_HOST_NAME);
}

TEST_CASE("a search needs at least one target") {
    CHECK_FALSE(fm::discoverHosts(searchFor({})));
}
