#pragma once

#include "Packets.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fm {
    constexpr std::string_view DEFAULT_HOST_NAME = "FreeMonitor";

    struct AnnounceInfo {
        std::uint16_t port = 0;
        std::uint8_t version = 0;
        std::string_view name;
    };

    std::string fitHostName(std::string_view name);

    std::size_t writeDiscover(std::uint32_t nonce, std::uint32_t seq, std::uint64_t timestampUs,
                              std::span<std::uint8_t> buffer) noexcept;
    std::optional<std::uint32_t> readDiscover(std::span<const std::uint8_t> packet) noexcept;

    std::size_t writeAnnounce(std::uint32_t nonce, std::uint16_t port, std::string_view name, std::uint32_t seq,
                              std::uint64_t timestampUs, std::span<std::uint8_t> buffer) noexcept;
    std::optional<AnnounceInfo> readAnnounce(std::span<const std::uint8_t> packet, std::uint32_t nonce) noexcept;
}
