#include "HostOptions.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace fm {
    namespace {
        constexpr int IPV4_OCTET_COUNT = 4;
        constexpr std::uint32_t MAX_OCTET = 255;
        constexpr std::uint32_t MAX_PORT = 65535;
        constexpr std::uint32_t MAX_JPEG_QUALITY = 100;
        constexpr std::array<std::pair<std::string_view, DisplayPlacement>, 4> NAMED_PLACEMENTS{{
                {"right", DisplayPlacement::RIGHT},
                {"left", DisplayPlacement::LEFT},
                {"above", DisplayPlacement::ABOVE},
                {"below", DisplayPlacement::BELOW},
        }};

        std::optional<std::uint32_t> parseNumber(std::string_view text, std::uint32_t maximum) {
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc() || end != text.data() + text.size() || value > maximum) {
                return std::nullopt;
            }
            return value;
        }

        std::unexpected<Error> invalidOption(std::string message) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, std::move(message)});
        }

        std::optional<std::int32_t> parseCoordinate(std::string_view text) {
            std::int32_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
                return std::nullopt;
            }
            return value;
        }

        bool parseSize(std::string_view text, DisplayMode& mode) {
            const std::size_t separator = text.find('x');
            if (separator == std::string_view::npos) {
                return false;
            }
            const auto width = parseNumber(text.substr(0, separator), MAX_DISPLAY_DIMENSION);
            const auto height = parseNumber(text.substr(separator + 1), MAX_DISPLAY_DIMENSION);
            if (!width || !height || *width == 0 || *height == 0) {
                return false;
            }
            mode.width = *width;
            mode.height = *height;
            return true;
        }

        bool parsePosition(std::string_view text, DisplayMode& mode) {
            for (const auto& [name, placement] : NAMED_PLACEMENTS) {
                if (text == name) {
                    mode.placement = placement;
                    return true;
                }
            }
            const std::size_t separator = text.find(',');
            if (separator == std::string_view::npos) {
                return false;
            }
            const auto x = parseCoordinate(text.substr(0, separator));
            const auto y = parseCoordinate(text.substr(separator + 1));
            if (!x || !y) {
                return false;
            }
            mode.placement = DisplayPlacement::EXPLICIT;
            mode.positionX = *x;
            mode.positionY = *y;
            return true;
        }
    }

    std::optional<Endpoint> parseEndpoint(std::string_view text) {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const auto port = parseNumber(text.substr(colon + 1), MAX_PORT);
        if (!port || *port == 0) {
            return std::nullopt;
        }

        std::uint32_t address = 0;
        std::string_view rest = text.substr(0, colon);
        for (int i = 0; i < IPV4_OCTET_COUNT; ++i) {
            const bool lastOctet = i == IPV4_OCTET_COUNT - 1;
            const std::size_t dot = rest.find('.');
            if (lastOctet != (dot == std::string_view::npos)) {
                return std::nullopt;
            }
            const auto octet = parseNumber(lastOctet ? rest : rest.substr(0, dot), MAX_OCTET);
            if (!octet) {
                return std::nullopt;
            }
            address = (address << 8) | *octet;
            if (!lastOctet) {
                rest = rest.substr(dot + 1);
            }
        }
        return Endpoint{address, static_cast<std::uint16_t>(*port)};
    }

    Result<HostOptions> parseHostOptions(std::span<const std::string_view> arguments, bool developmentModeAllowed) {
        HostOptions options;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const std::string_view option = arguments[i];
            if (option != "--seconds" && option != "--quality" && option != "--destination" && option != "--size" &&
                option != "--refresh" && option != "--position") {
                return invalidOption("Unknown option " + std::string(option));
            }
            if (i + 1 >= arguments.size()) {
                return invalidOption(std::string(option) + " needs a value");
            }
            const std::string_view value = arguments[++i];

            if (option == "--seconds") {
                const auto seconds = parseNumber(value, std::numeric_limits<std::uint32_t>::max());
                if (!seconds) {
                    return invalidOption("--seconds must be a whole number of seconds");
                }
                options.runSeconds = *seconds;
            } else if (option == "--quality") {
                const auto quality = parseNumber(value, MAX_JPEG_QUALITY);
                if (!quality || *quality == 0) {
                    return invalidOption("--quality must be 1 to 100");
                }
                options.jpegQuality = static_cast<std::uint8_t>(*quality);
            } else if (option == "--size") {
                if (!parseSize(value, options.displayMode)) {
                    return invalidOption("--size must be WIDTHxHEIGHT, each 1 to 16384, for example 2560x1600");
                }
            } else if (option == "--refresh") {
                const auto refreshRate = parseNumber(value, MAX_REFRESH_RATE_HZ);
                if (!refreshRate || *refreshRate == 0) {
                    return invalidOption("--refresh must be 1 to 240 Hz");
                }
                options.displayMode.refreshRateHz = *refreshRate;
            } else if (option == "--position") {
                if (!parsePosition(value, options.displayMode)) {
                    return invalidOption("--position must be right, left, above, below or X,Y, for example -1920,0");
                }
            } else {
                if (!developmentModeAllowed) {
                    return invalidOption("--destination is development mode, which a release build does not have");
                }
                const auto destination = parseEndpoint(value);
                if (!destination) {
                    return invalidOption("--destination must be IPv4:port, for example 127.0.0.1:50500");
                }
                options.destination = *destination;
            }
        }
        return options;
    }
}
