#include "CursorTracker.hpp"

#include <utility>

namespace fm {
    void CursorTracker::updateVisibility() noexcept {
        const bool visible = entered && (hotspotX != 0 || hotspotY != 0);
        if (visible != state.visible) {
            state.visible = visible;
            changed = true;
        }
    }

    void CursorTracker::enter() noexcept {
        entered = true;
        updateVisibility();
    }

    void CursorTracker::leave() noexcept {
        entered = false;
        updateVisibility();
    }

    void CursorTracker::moveTo(std::int32_t x, std::int32_t y) noexcept {
        if (x != state.x || y != state.y) {
            state.x = x;
            state.y = y;
            changed = true;
        }
    }

    void CursorTracker::setHotspot(std::int32_t x, std::int32_t y) noexcept {
        hotspotX = x;
        hotspotY = y;
        updateVisibility();
    }

    void CursorTracker::setImageId(std::uint32_t imageId) noexcept {
        if (imageId != state.imageId) {
            state.imageId = imageId;
            changed = true;
        }
    }

    bool CursorTracker::takeChange(CursorState& latest) noexcept {
        latest = state;
        return std::exchange(changed, false);
    }
}
