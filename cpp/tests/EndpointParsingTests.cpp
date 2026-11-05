#include "EndpointParsing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string_view>

TEST_CASE("parseEndpoint reads IPv4:port") {
    CHECK(fm::parseEndpoint("127.0.0.1:50500") == fm::Endpoint{fm::LOOPBACK_IP_ADDRESS, 50500});
    CHECK(fm::parseEndpoint("192.168.1.20:1") == fm::Endpoint{fm::makeIPAddress(192, 168, 1, 20), 1});
    CHECK(fm::parseEndpoint("255.255.255.255:65535") == fm::Endpoint{fm::makeIPAddress(255, 255, 255, 255), 65535});
    CHECK(fm::parseEndpoint("0.0.0.0:9") == fm::Endpoint{fm::ANY_IP_ADDRESS, 9});
}

TEST_CASE("parseEndpoint rejects anything else") {
    const std::string_view text =
            GENERATE("", "127.0.0.1", "127.0.0.1:", ":50500", "127.0.0:1", "127.0.0.1.1:5", "256.0.0.1:5",
                     "127.0.0.1:0", "127.0.0.1:65536", "1..2.3:5", "a.b.c.d:5", "127.0.0.1:50500x", "-1.0.0.1:5",
                     " 127.0.0.1:5", "127.0.0.1:+5", "localhost:50500", "::1:50500");
    CAPTURE(text);
    CHECK_FALSE(fm::parseEndpoint(text));
}
