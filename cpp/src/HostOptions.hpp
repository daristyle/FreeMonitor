#pragma once

#include "Encoder.hpp"
#include "Error.hpp"
#include "UDPSocket.hpp"
#include "VirtualDisplay.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fm {
    constexpr bool DEVELOPMENT_MODE_AVAILABLE = FM_DEVELOPMENT_MODE != 0;
    constexpr std::uint32_t DEFAULT_RUN_SECONDS = 10;
    constexpr std::uint32_t DEFAULT_DISPLAY_WIDTH = 1920;
    constexpr std::uint32_t DEFAULT_DISPLAY_HEIGHT = 1080;
    constexpr std::uint32_t MAX_DISPLAY_DIMENSION = 16384;
    constexpr std::uint32_t MAX_REFRESH_RATE_HZ = 240;

    struct HostOptions {
        std::uint32_t runSeconds = DEFAULT_RUN_SECONDS;
        std::uint8_t jpegQuality = EncoderConfig{}.jpegQuality;
        DisplayMode displayMode{.width = DEFAULT_DISPLAY_WIDTH, .height = DEFAULT_DISPLAY_HEIGHT};
        std::optional<Endpoint> destination;
    };

    std::optional<Endpoint> parseEndpoint(std::string_view text);
    Result<HostOptions> parseHostOptions(std::span<const std::string_view> arguments, bool developmentModeAllowed);
}
