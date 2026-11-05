#pragma once

#include "Packets.hpp"

#include <cstdint>

namespace fm {
    struct StreamInfo {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t fps = 0;
        Codec codec = Codec::JPEG;
        std::uint8_t features = 0;
    };
}
