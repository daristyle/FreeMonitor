#pragma once

#include "Encoder.hpp"
#include "Error.hpp"
#include "UDPSocket.hpp"
#include "VirtualDisplay.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fm {
    constexpr bool DEVELOPMENT_MODE_AVAILABLE = FM_DEVELOPMENT_MODE != 0;
    constexpr std::uint32_t DEFAULT_RUN_SECONDS = 10;
    constexpr std::uint32_t DEFAULT_DISPLAY_WIDTH = 1920;
    constexpr std::uint32_t DEFAULT_DISPLAY_HEIGHT = 1080;
    constexpr std::uint32_t MAX_DISPLAY_DIMENSION = 16384;
    constexpr std::uint32_t MAX_REFRESH_RATE_HZ = 240;
    constexpr std::uint32_t MAX_BITRATE_MBPS = 500;
    constexpr std::uint8_t DEFAULT_FEC_PERCENT = 30;

    enum class CursorChoice : std::uint8_t {
        AUTO,
        OVERLAY,
        EMBEDDED,
    };

    struct HostOptions {
        std::uint32_t runSeconds = DEFAULT_RUN_SECONDS;
        std::uint8_t jpegQuality = EncoderConfig{}.jpegQuality;
        EncoderChoice encoder = EncoderChoice::AUTO;
        std::uint32_t bitrateKbps = EncoderConfig{}.bitrateKbps;
        std::uint8_t fecPercent = DEFAULT_FEC_PERCENT;
        CursorChoice cursor = CursorChoice::AUTO;
        DisplayMode displayMode{.width = DEFAULT_DISPLAY_WIDTH, .height = DEFAULT_DISPLAY_HEIGHT};
        std::optional<Endpoint> destination;
        std::optional<std::string> timingLogPath;
        std::optional<std::string> hostName;
    };

    Result<HostOptions> parseHostOptions(std::span<const std::string_view> arguments, bool developmentModeAllowed);
}
