#include "EndpointParsing.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace fm {
    namespace {
        constexpr int IPV4_OCTET_COUNT = 4;
        constexpr std::uint32_t MAX_OCTET = 255;
        constexpr std::uint32_t MAX_PORT = 65535;

        std::optional<std::uint32_t> parseNumber(std::string_view text, std::uint32_t maximum) {
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc() || end != text.data() + text.size() || value > maximum) {
                return std::nullopt;
            }
            return value;
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
}
