#include "UDPSocket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::chrono::milliseconds RECEIVE_TIMEOUT{1000};
    const fm::Endpoint ANY_LOOPBACK_PORT{fm::LOOPBACK_IP_ADDRESS, 0};

    fm::UDPSocket openLoopbackSocket() {
        auto udpSocket = fm::UDPSocket::open(ANY_LOOPBACK_PORT);
        REQUIRE(udpSocket);
        return std::move(*udpSocket);
    }

    fm::Endpoint localEndpointOf(const fm::UDPSocket& udpSocket) {
        const auto endpoint = udpSocket.localEndpoint();
        REQUIRE(endpoint);
        return *endpoint;
    }
}

TEST_CASE("makeIPAddress builds the address in host byte order") {
    STATIC_REQUIRE(fm::makeIPAddress(127, 0, 0, 1) == 0x7F000001u);
    STATIC_REQUIRE(fm::makeIPAddress(192, 168, 1, 20) == 0xC0A80114u);
    STATIC_REQUIRE(fm::ANY_IP_ADDRESS == 0u);
}

TEST_CASE("open binds to a free port on loopback") {
    const auto udpSocket = openLoopbackSocket();
    const auto endpoint = localEndpointOf(udpSocket);
    CHECK(endpoint.address == fm::LOOPBACK_IP_ADDRESS);
    CHECK(endpoint.port != 0);
}

TEST_CASE("open fails when the port is already taken") {
    const auto first = openLoopbackSocket();
    const auto second = fm::UDPSocket::open(localEndpointOf(first));
    REQUIRE_FALSE(second);
    CHECK_FALSE(second.error().message.empty());
}

TEST_CASE("a datagram arrives intact with the sender's endpoint") {
    auto sender = openLoopbackSocket();
    auto receiver = openLoopbackSocket();
    const std::array<std::uint8_t, 5> message{1, 2, 3, 250, 255};

    REQUIRE(sender.sendTo(message, localEndpointOf(receiver)) == fm::SendStatus::SENT);

    std::array<std::uint8_t, 64> buffer{};
    fm::Endpoint from;
    const auto result = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
    REQUIRE(result.status == fm::ReceiveStatus::RECEIVED);
    REQUIRE(result.size == message.size());
    CHECK(std::equal(message.begin(), message.end(), buffer.begin()));
    CHECK(from == localEndpointOf(sender));
}

TEST_CASE("an empty datagram is a valid datagram") {
    auto sender = openLoopbackSocket();
    auto receiver = openLoopbackSocket();

    REQUIRE(sender.sendTo({}, localEndpointOf(receiver)) == fm::SendStatus::SENT);

    std::array<std::uint8_t, 16> buffer{};
    fm::Endpoint from;
    const auto result = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
    CHECK(result.status == fm::ReceiveStatus::RECEIVED);
    CHECK(result.size == 0);
}

TEST_CASE("receiveFrom returns NO_DATAGRAM when nothing arrives") {
    auto receiver = openLoopbackSocket();
    std::array<std::uint8_t, 16> buffer{};
    fm::Endpoint from;

    SECTION("zero timeout returns at once") {
        CHECK(receiver.receiveFrom(buffer, from, 0ms).status == fm::ReceiveStatus::NO_DATAGRAM);
    }
    SECTION("short timeout waits and gives up") {
        CHECK(receiver.receiveFrom(buffer, from, 20ms).status == fm::ReceiveStatus::NO_DATAGRAM);
    }
}

TEST_CASE("a datagram larger than the buffer is reported as TRUNCATED and discarded") {
    auto sender = openLoopbackSocket();
    auto receiver = openLoopbackSocket();
    const fm::Endpoint receiverEndpoint = localEndpointOf(receiver);
    const std::vector<std::uint8_t> large(100, 0xAB);
    const std::array<std::uint8_t, 3> small{7, 8, 9};

    REQUIRE(sender.sendTo(large, receiverEndpoint) == fm::SendStatus::SENT);
    REQUIRE(sender.sendTo(small, receiverEndpoint) == fm::SendStatus::SENT);

    std::array<std::uint8_t, 10> buffer{};
    fm::Endpoint from;
    const auto truncated = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
    CHECK(truncated.status == fm::ReceiveStatus::TRUNCATED);
    CHECK(truncated.size == buffer.size());

    const auto next = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
    REQUIRE(next.status == fm::ReceiveStatus::RECEIVED);
    REQUIRE(next.size == small.size());
    CHECK(std::equal(small.begin(), small.end(), buffer.begin()));
}

TEST_CASE("one socket serves several peers") {
    auto host = openLoopbackSocket();
    auto firstTablet = openLoopbackSocket();
    auto secondTablet = openLoopbackSocket();
    const fm::Endpoint hostEndpoint = localEndpointOf(host);

    const std::array<std::uint8_t, 1> firstHello{1};
    const std::array<std::uint8_t, 1> secondHello{2};
    REQUIRE(firstTablet.sendTo(firstHello, hostEndpoint) == fm::SendStatus::SENT);
    REQUIRE(secondTablet.sendTo(secondHello, hostEndpoint) == fm::SendStatus::SENT);

    std::array<std::uint8_t, 16> buffer{};
    for (int i = 0; i < 2; ++i) {
        fm::Endpoint from;
        const auto result = host.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
        REQUIRE(result.status == fm::ReceiveStatus::RECEIVED);
        REQUIRE(result.size == 1);
        const std::array<std::uint8_t, 1> reply{static_cast<std::uint8_t>(buffer[0] + 100)};
        REQUIRE(host.sendTo(reply, from) == fm::SendStatus::SENT);
    }

    fm::Endpoint from;
    REQUIRE(firstTablet.receiveFrom(buffer, from, RECEIVE_TIMEOUT).status == fm::ReceiveStatus::RECEIVED);
    CHECK(buffer[0] == 101);
    CHECK(from == hostEndpoint);
    REQUIRE(secondTablet.receiveFrom(buffer, from, RECEIVE_TIMEOUT).status == fm::ReceiveStatus::RECEIVED);
    CHECK(buffer[0] == 102);
    CHECK(from == hostEndpoint);
}

TEST_CASE("sending to port 0 fails") {
    auto sender = openLoopbackSocket();
    const std::array<std::uint8_t, 1> message{1};
    CHECK(sender.sendTo(message, fm::Endpoint{fm::LOOPBACK_IP_ADDRESS, 0}) == fm::SendStatus::SEND_FAILED);
}

TEST_CASE("a moved socket keeps working and the moved-from one closes nothing") {
    auto original = openLoopbackSocket();
    const fm::Endpoint endpoint = localEndpointOf(original);

    fm::UDPSocket moved = std::move(original);
    CHECK(localEndpointOf(moved) == endpoint);

    auto sender = openLoopbackSocket();
    const std::array<std::uint8_t, 2> message{4, 2};
    REQUIRE(sender.sendTo(message, endpoint) == fm::SendStatus::SENT);
    std::array<std::uint8_t, 16> buffer{};
    fm::Endpoint from;
    CHECK(moved.receiveFrom(buffer, from, RECEIVE_TIMEOUT).status == fm::ReceiveStatus::RECEIVED);
}

TEST_CASE("a moved-from socket reports failures instead of using a stale handle") {
    auto original = openLoopbackSocket();
    const fm::UDPSocket moved = std::move(original);

    CHECK_FALSE(original.localEndpoint());

    const std::array<std::uint8_t, 1> message{1};
    CHECK(original.sendTo(message, localEndpointOf(moved)) == fm::SendStatus::SEND_FAILED);

    std::array<std::uint8_t, 16> buffer{};
    fm::Endpoint from;
    CHECK(original.receiveFrom(buffer, from, 0ms).status == fm::ReceiveStatus::RECEIVE_FAILED);
}

TEST_CASE("move assignment closes the socket it replaces") {
    auto target = openLoopbackSocket();
    const fm::Endpoint replacedEndpoint = localEndpointOf(target);

    target = openLoopbackSocket();

    CHECK(fm::UDPSocket::open(replacedEndpoint));
}

TEST_CASE("buffer sizes can be raised") {
    constexpr int SMALL_BUFFER_BYTES = 16 * 1024;
    constexpr int LARGE_BUFFER_BYTES = 1024 * 1024;
    auto udpSocket = openLoopbackSocket();

    SECTION("receive buffer") {
        const auto smallBytes = udpSocket.setReceiveBufferSize(SMALL_BUFFER_BYTES);
        REQUIRE(smallBytes);
        const auto largeBytes = udpSocket.setReceiveBufferSize(LARGE_BUFFER_BYTES);
        REQUIRE(largeBytes);
        CHECK(*largeBytes > *smallBytes);
    }
    SECTION("send buffer") {
        const auto smallBytes = udpSocket.setSendBufferSize(SMALL_BUFFER_BYTES);
        REQUIRE(smallBytes);
        const auto largeBytes = udpSocket.setSendBufferSize(LARGE_BUFFER_BYTES);
        REQUIRE(largeBytes);
        CHECK(*largeBytes > *smallBytes);
    }
}
