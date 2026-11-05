#include "HeartbeatEcho.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

using namespace std::chrono_literals;

namespace {
    using Clock = std::chrono::steady_clock;

    Clock::time_point at(std::chrono::microseconds time) {
        return Clock::time_point{} + time;
    }

    fm::Heartbeat heartbeatFrom(std::uint64_t peerSentUs, std::uint64_t echoedUs, std::uint32_t delayUs) {
        fm::Heartbeat heartbeat;
        heartbeat.commonHeader.timestampUs = peerSentUs;
        heartbeat.echoTimestampUs = echoedUs;
        heartbeat.echoDelayUs = delayUs;
        return heartbeat;
    }
}

TEST_CASE("a peer clock that runs ahead gives a positive offset") {
    fm::HeartbeatEcho echo;
    echo.received(heartbeatFrom(6'012'000, 1'000'000, 10'000), at(1'014'000us));

    const auto sample = echo.clockSample();
    REQUIRE(sample);
    CHECK(sample->peerClockOffset == 5'000'000us);
    CHECK(sample->roundTripTime == 4'000us);
    CHECK(sample->receivedTime == at(1'014'000us));
    CHECK(echo.roundTripTime() == 4'000us);
}

TEST_CASE("a peer clock that runs behind gives a negative offset") {
    fm::HeartbeatEcho echo;
    echo.received(heartbeatFrom(7'012'000, 10'000'000, 10'000), at(10'014'000us));

    const auto sample = echo.clockSample();
    REQUIRE(sample);
    CHECK(sample->peerClockOffset == -3'000'000us);
}

TEST_CASE("an uneven network shifts the offset by half the difference between the two directions") {
    fm::HeartbeatEcho echo;
    echo.received(heartbeatFrom(6'011'000, 1'000'000, 10'000), at(1'016'000us));

    const auto sample = echo.clockSample();
    REQUIRE(sample);
    CHECK(sample->peerClockOffset == 4'998'000us);
    CHECK(sample->roundTripTime == 6'000us);
}

TEST_CASE("a heartbeat that echoes nothing gives no clock sample") {
    fm::HeartbeatEcho echo;
    echo.received(heartbeatFrom(6'000'000, 0, 0), at(1'000'000us));

    CHECK_FALSE(echo.clockSample());
    CHECK_FALSE(echo.roundTripTime());
}

TEST_CASE("a clock sample stays until the next one and reset clears it") {
    fm::HeartbeatEcho echo;
    echo.received(heartbeatFrom(6'012'000, 1'000'000, 10'000), at(1'014'000us));
    echo.received(heartbeatFrom(6'512'000, 0, 0), at(1'514'000us));
    REQUIRE(echo.clockSample());
    CHECK(echo.clockSample()->receivedTime == at(1'014'000us));

    echo.reset();
    CHECK_FALSE(echo.clockSample());
}

TEST_CASE("timestamps from the network cannot overflow the offset") {
    const std::uint64_t peerSentUs =
            GENERATE(std::uint64_t{1}, std::uint64_t{1} << 63, std::numeric_limits<std::uint64_t>::max());
    fm::HeartbeatEcho echo;
    echo.received(heartbeatFrom(peerSentUs, 1'000, std::numeric_limits<std::uint32_t>::max()), at(10'000'000'000us));

    CHECK(echo.clockSample());
}
