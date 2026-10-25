#include "HostOptions.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string_view>
#include <utility>
#include <vector>

namespace {
    fm::Result<fm::HostOptions> parse(std::vector<std::string_view> arguments, bool developmentModeAllowed = true) {
        return fm::parseHostOptions(arguments, developmentModeAllowed);
    }
}

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

TEST_CASE("parseHostOptions uses the defaults without arguments") {
    const auto options = parse({});
    REQUIRE(options);
    CHECK(options->runSeconds == fm::DEFAULT_RUN_SECONDS);
    CHECK(options->jpegQuality == fm::EncoderConfig{}.jpegQuality);
    CHECK(options->displayMode.width == fm::DEFAULT_DISPLAY_WIDTH);
    CHECK(options->displayMode.height == fm::DEFAULT_DISPLAY_HEIGHT);
    CHECK(options->displayMode.refreshRateHz == fm::DisplayMode{}.refreshRateHz);
    CHECK(options->displayMode.placement == fm::DisplayPlacement::RIGHT);
    CHECK_FALSE(options->timingLogPath);
    CHECK_FALSE(options->destination);
}

TEST_CASE("parseHostOptions reads the virtual monitor's size, refresh rate and position") {
    const auto options = parse({"--size", "2560x1600", "--refresh", "120", "--position", "-2560,40"});
    REQUIRE(options);
    CHECK(options->displayMode.width == 2560);
    CHECK(options->displayMode.height == 1600);
    CHECK(options->displayMode.refreshRateHz == 120);
    CHECK(options->displayMode.placement == fm::DisplayPlacement::EXPLICIT);
    CHECK(options->displayMode.positionX == -2560);
    CHECK(options->displayMode.positionY == 40);
}

TEST_CASE("parseHostOptions reads a relative position") {
    const auto [text, placement] =
            GENERATE(std::pair{"right", fm::DisplayPlacement::RIGHT}, std::pair{"left", fm::DisplayPlacement::LEFT},
                     std::pair{"above", fm::DisplayPlacement::ABOVE}, std::pair{"below", fm::DisplayPlacement::BELOW});
    CAPTURE(text);
    const auto options = parse({"--position", text});
    REQUIRE(options);
    CHECK(options->displayMode.placement == placement);
}

TEST_CASE("parseHostOptions reads every option") {
    const auto options = parse({"--seconds", "0", "--quality", "55", "--destination", "10.0.0.2:50500", "--timing-log",
                                "runs/host-timing.csv"});
    REQUIRE(options);
    CHECK(options->runSeconds == 0);
    CHECK(options->jpegQuality == 55);
    CHECK(options->destination == fm::Endpoint{fm::makeIPAddress(10, 0, 0, 2), 50500});
    CHECK(options->timingLogPath == "runs/host-timing.csv");
}

TEST_CASE("parseHostOptions rejects bad arguments") {
    const auto arguments = GENERATE(
            std::vector<std::string_view>{"--fps", "60"}, std::vector<std::string_view>{"seconds", "5"},
            std::vector<std::string_view>{"--seconds"}, std::vector<std::string_view>{"--seconds", "-1"},
            std::vector<std::string_view>{"--seconds", "ten"}, std::vector<std::string_view>{"--seconds", "4294967296"},
            std::vector<std::string_view>{"--quality", "0"}, std::vector<std::string_view>{"--quality", "101"},
            std::vector<std::string_view>{"--quality", ""}, std::vector<std::string_view>{"--destination", "127.0.0.1"},
            std::vector<std::string_view>{"--seconds", "5", "--quality"},
            std::vector<std::string_view>{"--size", "1920"}, std::vector<std::string_view>{"--size", "0x1080"},
            std::vector<std::string_view>{"--size", "1920x16385"},
            std::vector<std::string_view>{"--size", "1920x1080x2"}, std::vector<std::string_view>{"--refresh", "0"},
            std::vector<std::string_view>{"--refresh", "241"}, std::vector<std::string_view>{"--position", "middle"},
            std::vector<std::string_view>{"--position", "1920"}, std::vector<std::string_view>{"--position", "1920,"},
            std::vector<std::string_view>{"--position", "Right"}, std::vector<std::string_view>{"--position", "1e3,0"},
            std::vector<std::string_view>{"--timing-log"}, std::vector<std::string_view>{"--timing-log", ""});
    const auto options = parse(arguments);
    REQUIRE_FALSE(options);
    CHECK(options.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    CHECK_FALSE(options.error().message.empty());
}

TEST_CASE("--destination is refused when development mode is not allowed") {
    const auto options = parse({"--destination", "127.0.0.1:50500"}, false);
    REQUIRE_FALSE(options);
    CHECK(options.error().message.find("release") != std::string::npos);

    CHECK(parse({"--seconds", "5", "--quality", "70"}, false));
    CHECK(parse({"--timing-log", "host-timing.csv"}, false));
}
