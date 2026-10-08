#pragma once

#include <chrono>
#include <cstdint>
#include <span>

namespace fm {
    enum class PixelFormat : std::uint8_t {
        BGRA8,
        RGBA8,
    };

    constexpr std::uint32_t bytesPerPixel(PixelFormat format) {
        switch (format) {
            case PixelFormat::BGRA8:
            case PixelFormat::RGBA8:
                return 4;
        }
        return 0;
    }

    struct CapturedFrame {
        const std::uint8_t* pixels = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        PixelFormat format = PixelFormat::BGRA8;
        std::chrono::steady_clock::time_point captureTime;
    };

    struct EncodedFrame {
        std::span<const std::uint8_t> data;
        bool keyframe = false;
        std::chrono::steady_clock::time_point captureTime;
    };

}
