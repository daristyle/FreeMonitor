#include "DisplayLayout.hpp"

#include <algorithm>

namespace fm {
    namespace {
        std::int32_t right(const MonitorLayout& monitor) {
            return monitor.x + static_cast<std::int32_t>(monitor.width);
        }

        std::int32_t bottom(const MonitorLayout& monitor) {
            return monitor.y + static_cast<std::int32_t>(monitor.height);
        }
    }

    DisplayPosition placeDisplay(std::span<const MonitorLayout> monitors, const DisplayMode& mode) {
        if (mode.placement == DisplayPlacement::EXPLICIT) {
            return DisplayPosition{mode.positionX, mode.positionY};
        }
        if (monitors.empty()) {
            return DisplayPosition{};
        }

        const auto width = static_cast<std::int32_t>(mode.width);
        const auto height = static_cast<std::int32_t>(mode.height);
        switch (mode.placement) {
            case DisplayPlacement::RIGHT: {
                const auto& edge = *std::ranges::max_element(monitors, {}, right);
                return DisplayPosition{right(edge), edge.y};
            }
            case DisplayPlacement::LEFT: {
                const auto& edge = *std::ranges::min_element(monitors, {}, &MonitorLayout::x);
                return DisplayPosition{edge.x - width, edge.y};
            }
            case DisplayPlacement::ABOVE: {
                const auto& edge = *std::ranges::min_element(monitors, {}, &MonitorLayout::y);
                return DisplayPosition{edge.x, edge.y - height};
            }
            case DisplayPlacement::BELOW: {
                const auto& edge = *std::ranges::max_element(monitors, {}, bottom);
                return DisplayPosition{edge.x, bottom(edge)};
            }
            case DisplayPlacement::EXPLICIT:
                break;
        }
        return DisplayPosition{mode.positionX, mode.positionY};
    }
}
