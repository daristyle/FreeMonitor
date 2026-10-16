#include "SerialNumber.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

namespace {
    constexpr std::uint32_t MAX_SERIAL = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint32_t HALF_RANGE = 0x80000000u;
}

TEST_CASE("equal serial numbers are neither newer nor older") {
    STATIC_REQUIRE(fm::serialDifference(42, 42) == 0);
    STATIC_REQUIRE_FALSE(fm::isNewer(42, 42));
}

TEST_CASE("a larger serial number nearby is newer") {
    STATIC_REQUIRE(fm::serialDifference(43, 42) == 1);
    STATIC_REQUIRE(fm::serialDifference(42, 43) == -1);
    STATIC_REQUIRE(fm::isNewer(43, 42));
    STATIC_REQUIRE_FALSE(fm::isNewer(42, 43));
}

TEST_CASE("the order survives the wrap from 2^32 - 1 to 0") {
    STATIC_REQUIRE(fm::serialDifference(0, MAX_SERIAL) == 1);
    STATIC_REQUIRE(fm::serialDifference(MAX_SERIAL, 0) == -1);
    STATIC_REQUIRE(fm::serialDifference(5, MAX_SERIAL - 4) == 10);
    STATIC_REQUIRE(fm::isNewer(0, MAX_SERIAL));
    STATIC_REQUIRE_FALSE(fm::isNewer(MAX_SERIAL, 0));
}

TEST_CASE("up to 2^31 - 1 ahead still counts as newer") {
    STATIC_REQUIRE(fm::serialDifference(HALF_RANGE - 1, 0) == std::numeric_limits<std::int32_t>::max());
    STATIC_REQUIRE(fm::isNewer(HALF_RANGE - 1, 0));
    STATIC_REQUIRE(fm::isNewer(HALF_RANGE + 9, 10));
}

TEST_CASE("exactly 2^31 apart cannot be ordered, so neither side is newer") {
    STATIC_REQUIRE(fm::serialDifference(HALF_RANGE, 0) == std::numeric_limits<std::int32_t>::min());
    STATIC_REQUIRE(fm::serialDifference(0, HALF_RANGE) == std::numeric_limits<std::int32_t>::min());
    STATIC_REQUIRE_FALSE(fm::isNewer(HALF_RANGE, 0));
    STATIC_REQUIRE_FALSE(fm::isNewer(0, HALF_RANGE));
}

TEST_CASE("more than 2^31 ahead looks older") {
    STATIC_REQUIRE(fm::serialDifference(HALF_RANGE + 1, 0) == -static_cast<std::int32_t>(HALF_RANGE - 1));
    STATIC_REQUIRE_FALSE(fm::isNewer(HALF_RANGE + 1, 0));
    STATIC_REQUIRE(fm::isNewer(0, HALF_RANGE + 1));
}
