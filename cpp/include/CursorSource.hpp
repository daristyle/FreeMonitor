#pragma once

#include "CaptureSource.hpp"
#include "Error.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace fm {
    constexpr std::uint16_t MAX_CURSOR_IMAGE_SIDE = 256;

    struct CursorImage {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t hotspotX = 0;
        std::uint16_t hotspotY = 0;
        std::vector<std::uint8_t> pixels;
    };

    struct CursorState {
        std::int32_t x = 0;
        std::int32_t y = 0;
        bool visible = false;
        std::uint32_t imageId = 0;
    };

    enum class CursorStatus : std::uint8_t {
        CHANGED,
        UNCHANGED,
        CURSOR_LOST,
    };

    class CursorSource {
    protected:
        CursorSource() = default;

    public:
        virtual ~CursorSource() = default;

        CursorSource(const CursorSource&) = delete;
        CursorSource& operator=(const CursorSource&) = delete;

        virtual Result<> start(const CaptureTarget& target) = 0;
        virtual void stop() = 0;
        virtual CursorStatus waitForChange(CursorState& state, std::chrono::milliseconds timeout) noexcept = 0;
        virtual void wake() noexcept = 0;
        virtual const CursorImage& image() const noexcept = 0;
    };

    std::unique_ptr<CursorSource> createCursorSource();

}
