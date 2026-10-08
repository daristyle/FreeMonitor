#pragma once

#include "CaptureSource.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace fm {
    class StubCaptureSource final : public CaptureSource {
    private:
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t stride;
        std::vector<std::uint8_t> pixels;
        std::uint64_t frameIndex = 0;
        bool started = false;
        bool frameHeld = false;

    public:
        static constexpr std::uint32_t DEFAULT_WIDTH = 1920;
        static constexpr std::uint32_t DEFAULT_HEIGHT = 1080;
        static constexpr std::uint32_t ROW_PADDING_BYTES = 64;
        static constexpr PixelFormat PIXEL_FORMAT = PixelFormat::BGRA8;

        explicit StubCaptureSource(std::uint32_t width = DEFAULT_WIDTH, std::uint32_t height = DEFAULT_HEIGHT);

        Result<> start(const CaptureTarget& target) override;
        void stop() override;
        CaptureStatus acquireFrame(CapturedFrame& frame, std::chrono::milliseconds timeout) noexcept override;
        void releaseFrame() noexcept override;

        static constexpr std::array<std::uint8_t, 4> patternPixel(std::uint32_t x, std::uint32_t y,
                                                                  std::uint64_t frameIndex) {
            return {static_cast<std::uint8_t>(x + frameIndex), static_cast<std::uint8_t>(y),
                    static_cast<std::uint8_t>(frameIndex), 0xFF};
        }
    };

}
