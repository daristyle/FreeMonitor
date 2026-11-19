#pragma once

#include "CursorSource.hpp"

#include <cstdint>

namespace fm {
    class CursorTracker {
    private:
        CursorState state;
        bool entered = false;
        std::int32_t hotspotX = 0;
        std::int32_t hotspotY = 0;
        bool changed = false;

        void updateVisibility() noexcept;

    public:
        void enter() noexcept;
        void leave() noexcept;
        void moveTo(std::int32_t x, std::int32_t y) noexcept;
        void setHotspot(std::int32_t x, std::int32_t y) noexcept;
        void setImageId(std::uint32_t imageId) noexcept;
        bool takeChange(CursorState& latest) noexcept;

        const CursorState& current() const noexcept {
            return state;
        }
    };
}
