#include "UDPSocket.hpp"
#include "posix/ReceiveTimestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>
#include <utility>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::chrono::milliseconds RECEIVE_TIMEOUT{1000};
    constexpr std::chrono::milliseconds WAIT_IN_SOCKET{50};
    constexpr std::chrono::milliseconds TOLERANCE{5};
    constexpr std::chrono::milliseconds PROBE_WAIT{10};
    constexpr int MAX_PROBES = 50;
    const fm::Endpoint ANY_LOOPBACK_PORT{fm::LOOPBACK_IP_ADDRESS, 0};

    const Clock::time_point READ_TIME = Clock::time_point{} + std::chrono::seconds(5000);

    timespec wallClock(std::time_t seconds, long nanoseconds) {
        timespec time{};
        time.tv_sec = seconds;
        time.tv_nsec = nanoseconds;
        return time;
    }

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

    void waitForReceiveTimestamps(fm::UDPSocket& sender, fm::UDPSocket& receiver) {
        const std::array<std::uint8_t, 1> probe{0};
        std::array<std::uint8_t, 16> buffer{};
        fm::Endpoint from;
        for (int attempt = 0; attempt < MAX_PROBES; ++attempt) {
            REQUIRE(sender.sendTo(probe, localEndpointOf(receiver)) == fm::SendStatus::SENT);
            std::this_thread::sleep_for(PROBE_WAIT);
            const auto received = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
            REQUIRE(received.status == fm::ReceiveStatus::RECEIVED);
            if (Clock::now() - received.arrivalTime >= PROBE_WAIT / 2) {
                return;
            }
        }
        FAIL("the kernel never attached a receive timestamp");
    }
}

TEST_CASE("a datagram reports when the kernel received it, not when it was read") {
    auto sender = openLoopbackSocket();
    auto receiver = openLoopbackSocket();
    waitForReceiveTimestamps(sender, receiver);
    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
    const auto beforeSend = Clock::now();
    REQUIRE(sender.sendTo(payload, localEndpointOf(receiver)) == fm::SendStatus::SENT);
    std::this_thread::sleep_for(WAIT_IN_SOCKET);

    std::array<std::uint8_t, 16> buffer{};
    fm::Endpoint from;
    const auto received = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
    const auto afterRead = Clock::now();
    REQUIRE(received.status == fm::ReceiveStatus::RECEIVED);
    CHECK(received.arrivalTime >= beforeSend - TOLERANCE);
    CHECK(received.arrivalTime <= afterRead);
    CHECK(afterRead - received.arrivalTime >= WAIT_IN_SOCKET - TOLERANCE);
}

TEST_CASE("a truncated datagram also reports its arrival time") {
    auto sender = openLoopbackSocket();
    auto receiver = openLoopbackSocket();
    waitForReceiveTimestamps(sender, receiver);
    const std::array<std::uint8_t, 32> payload{};
    REQUIRE(sender.sendTo(payload, localEndpointOf(receiver)) == fm::SendStatus::SENT);
    std::this_thread::sleep_for(WAIT_IN_SOCKET);

    std::array<std::uint8_t, 8> buffer{};
    fm::Endpoint from;
    const auto received = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
    REQUIRE(received.status == fm::ReceiveStatus::TRUNCATED);
    CHECK(Clock::now() - received.arrivalTime >= WAIT_IN_SOCKET - TOLERANCE);
}

TEST_CASE("the arrival is the read time minus the packet's age on the wall clock") {
    using namespace std::chrono_literals;
    CHECK(fm::arrivalFromKernelTime(READ_TIME, wallClock(100, 20'000'000), wallClock(100, 12'000'000)) ==
          READ_TIME - 8ms);
    CHECK(fm::arrivalFromKernelTime(READ_TIME, wallClock(101, 100'000'000), wallClock(100, 900'000'000)) ==
          READ_TIME - 200ms);
    CHECK(fm::arrivalFromKernelTime(READ_TIME, wallClock(101, 0), wallClock(100, 0)) == READ_TIME - 1s);
}

TEST_CASE("an implausible packet age falls back to the read time") {
    CHECK(fm::arrivalFromKernelTime(READ_TIME, wallClock(100, 0), wallClock(100, 5'000'000)) == READ_TIME);
    CHECK(fm::arrivalFromKernelTime(READ_TIME, wallClock(100, 0), wallClock(100, 0)) == READ_TIME);
    CHECK(fm::arrivalFromKernelTime(READ_TIME, wallClock(101, 1), wallClock(100, 0)) == READ_TIME);
    CHECK(fm::arrivalFromKernelTime(READ_TIME, wallClock(3700, 20'000'000), wallClock(100, 12'000'000)) == READ_TIME);
}
