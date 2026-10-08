#include "stub/StubCaptureSource.hpp"

#include <cassert>
#include <cstddef>

namespace fm {
    StubCaptureSource::StubCaptureSource(std::uint32_t width, std::uint32_t height)
        : width(width), height(height), stride(width * bytesPerPixel(PIXEL_FORMAT) + ROW_PADDING_BYTES) {}

    Result<> StubCaptureSource::start(const CaptureTarget& target) {
        if (started) {
            return std::unexpected(Error{ErrorCode::INVALID_STATE, "Capture is already started"});
        }
        if (target.outputName.empty()) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Capture target has no output name"});
        }
        if (width == 0 || height == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Stub capture size must not be zero"});
        }

        pixels.assign(static_cast<std::size_t>(stride) * height, 0);
        frameIndex = 0;
        frameHeld = false;
        started = true;
        return {};
    }

    void StubCaptureSource::stop() {
        started = false;
        frameHeld = false;
        pixels.clear();
        pixels.shrink_to_fit();
    }

    CaptureStatus StubCaptureSource::acquireFrame(CapturedFrame& frame, std::chrono::milliseconds) noexcept {
        if (!started) {
            return CaptureStatus::CAPTURE_LOST;
        }
        assert(!frameHeld && "releaseFrame() must be called before the next acquireFrame()");

        constexpr std::size_t pixelSize = bytesPerPixel(PIXEL_FORMAT);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * stride;
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto color = patternPixel(x, y, frameIndex);
                std::uint8_t* pixel = row + x * pixelSize;
                pixel[0] = color[0];
                pixel[1] = color[1];
                pixel[2] = color[2];
                pixel[3] = color[3];
            }
        }

        frame.pixels = pixels.data();
        frame.width = width;
        frame.height = height;
        frame.stride = stride;
        frame.format = PIXEL_FORMAT;
        frame.captureTime = std::chrono::steady_clock::now();

        ++frameIndex;
        frameHeld = true;
        return CaptureStatus::FRAME_READY;
    }

    void StubCaptureSource::releaseFrame() noexcept {
        frameHeld = false;
    }

}
