#include "DiscoveryPackets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {
    constexpr std::uint32_t NONCE = 0xC0FFEE01;
    constexpr std::uint16_t PORT = 50500;

    std::vector<std::uint8_t> announceBytes(std::string_view name, std::uint16_t port = PORT) {
        std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
        const std::size_t size = fm::writeAnnounce(NONCE, port, name, 7, 1000, buffer);
        REQUIRE(size > 0);
        return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)};
    }

    std::vector<std::uint8_t> discoverBytes() {
        std::array<std::uint8_t, fm::Discover::SIZE> buffer{};
        REQUIRE(fm::writeDiscover(NONCE, 3, 1000, buffer) == fm::Discover::SIZE);
        return {buffer.begin(), buffer.end()};
    }
}

TEST_CASE("fitHostName keeps short names, falls back for an empty one and cuts long ones between characters") {
    CHECK(fm::fitHostName("fedora-laptop") == "fedora-laptop");
    CHECK(fm::fitHostName("") == fm::DEFAULT_HOST_NAME);
    CHECK(fm::fitHostName(std::string(fm::MAX_HOST_NAME_SIZE, 'a')).size() == fm::MAX_HOST_NAME_SIZE);
    CHECK(fm::fitHostName(std::string(fm::MAX_HOST_NAME_SIZE + 5, 'a')).size() == fm::MAX_HOST_NAME_SIZE);

    const std::string umlauts = std::string(fm::MAX_HOST_NAME_SIZE - 1, 'a') + "\xC3\xA4\xC3\xA4";
    CHECK(fm::fitHostName(umlauts) == std::string(fm::MAX_HOST_NAME_SIZE - 1, 'a'));
}

TEST_CASE("a DISCOVER is read back with its nonce, whatever its version") {
    auto bytes = discoverBytes();
    CHECK(fm::readDiscover(bytes) == NONCE);
    bytes[2] = 9;
    CHECK(fm::readDiscover(bytes) == NONCE);
}

TEST_CASE("a DISCOVER that breaks the rules is not answered") {
    auto bytes = discoverBytes();
    SECTION("wrong size") {
        bytes.push_back(0);
    }
    SECTION("wrong magic") {
        bytes[0] = 0;
    }
    SECTION("another type") {
        bytes[3] = static_cast<std::uint8_t>(fm::PacketType::ANNOUNCE);
    }
    SECTION("a session id") {
        bytes[7] = 1;
    }
    SECTION("headerReserved set") {
        bytes[15] = 1;
    }
    CHECK_FALSE(fm::readDiscover(bytes));
}

TEST_CASE("an ANNOUNCE carries the nonce, port, version and name") {
    const auto bytes = announceBytes("fedora-laptop");
    CHECK(bytes.size() == fm::Announce::SIZE + 13);
    const auto info = fm::readAnnounce(bytes, NONCE);
    REQUIRE(info);
    CHECK(info->port == PORT);
    CHECK(info->version == fm::PROTOCOL_VERSION);
    CHECK(info->name == "fedora-laptop");
}

TEST_CASE("an ANNOUNCE of another version is still read, so the tablet can say it is incompatible") {
    auto bytes = announceBytes("old-host");
    bytes[2] = 2;
    const auto info = fm::readAnnounce(bytes, NONCE);
    REQUIRE(info);
    CHECK(info->version == 2);
}

TEST_CASE("an ANNOUNCE that breaks the rules or belongs to another search is dropped") {
    auto bytes = announceBytes("host");
    SECTION("another search") {
        CHECK_FALSE(fm::readAnnounce(bytes, NONCE + 1));
        return;
    }
    SECTION("no name") {
        bytes.resize(fm::Announce::SIZE);
    }
    SECTION("a name longer than MAX_HOST_NAME_SIZE") {
        bytes.resize(fm::Announce::SIZE + fm::MAX_HOST_NAME_SIZE + 1, 'x');
    }
    SECTION("port 0") {
        bytes[28] = 0;
        bytes[29] = 0;
    }
    SECTION("reserved set") {
        bytes[31] = 1;
    }
    SECTION("a session id") {
        bytes[4] = 1;
    }
    CHECK_FALSE(fm::readAnnounce(bytes, NONCE));
}

TEST_CASE("writeAnnounce refuses a name or port the spec does not allow") {
    std::array<std::uint8_t, fm::MAX_PACKET_SIZE> buffer{};
    CHECK(fm::writeAnnounce(NONCE, PORT, "", 1, 1, buffer) == 0);
    CHECK(fm::writeAnnounce(NONCE, PORT, std::string(fm::MAX_HOST_NAME_SIZE + 1, 'a'), 1, 1, buffer) == 0);
    CHECK(fm::writeAnnounce(NONCE, 0, "host", 1, 1, buffer) == 0);
    CHECK(fm::writeAnnounce(NONCE, PORT, std::string(fm::MAX_HOST_NAME_SIZE, 'a'), 1, 1, buffer) ==
          fm::Announce::SIZE + fm::MAX_HOST_NAME_SIZE);
}
