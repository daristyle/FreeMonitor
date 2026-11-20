#include "RateLimiter.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

namespace {
    const std::chrono::steady_clock::time_point START{100s};
}

TEST_CASE("the limiter allows maxEvents in a window and refuses the next one") {
    fm::RateLimiter limiter(3, 1000ms);
    CHECK(limiter.allow(START));
    CHECK(limiter.allow(START + 10ms));
    CHECK(limiter.allow(START + 20ms));
    CHECK_FALSE(limiter.allow(START + 999ms));
}

TEST_CASE("an event is allowed again once the oldest allowed one is a whole window old") {
    fm::RateLimiter limiter(2, 1000ms);
    REQUIRE(limiter.allow(START));
    REQUIRE(limiter.allow(START + 500ms));
    CHECK_FALSE(limiter.allow(START + 999ms));
    CHECK(limiter.allow(START + 1000ms));
    CHECK_FALSE(limiter.allow(START + 1499ms));
    CHECK(limiter.allow(START + 1500ms));
}

TEST_CASE("refused events do not count against the window") {
    fm::RateLimiter limiter(1, 1000ms);
    REQUIRE(limiter.allow(START));
    for (int i = 1; i < 10; ++i) {
        CHECK_FALSE(limiter.allow(START + std::chrono::milliseconds(i * 100 - 1)));
    }
    CHECK(limiter.allow(START + 1000ms));
}

TEST_CASE("a limiter for zero events allows nothing") {
    fm::RateLimiter limiter(0, 1000ms);
    CHECK_FALSE(limiter.allow(START));
}
