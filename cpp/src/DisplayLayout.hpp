#pragma once

#include "VirtualDisplay.hpp"

#include <cstdint>
#include <span>

namespace fm {
    struct MonitorLayout {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        friend bool operator==(const MonitorLayout&, const MonitorLayout&) = default;
    };

    struct DisplayPosition {
        std::int32_t x = 0;
        std::int32_t y = 0;

        friend bool operator==(const DisplayPosition&, const DisplayPosition&) = default;
    };

    DisplayPosition placeDisplay(std::span<const MonitorLayout> monitors, const DisplayMode& mode);
}
