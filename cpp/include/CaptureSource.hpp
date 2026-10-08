#pragma once

#include "Error.hpp"
#include "Frame.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace fm {
    struct CaptureTarget {
        std::string outputName;
    };

    enum class CaptureStatus : std::uint8_t {
        FRAME_READY,
        NO_NEW_FRAME,
        CAPTURE_LOST,
    };

    class CaptureSource {
    protected:
        CaptureSource() = default;

    public:
        virtual ~CaptureSource() = default;

        CaptureSource(const CaptureSource&) = delete;
        CaptureSource& operator=(const CaptureSource&) = delete;

        virtual Result<> start(const CaptureTarget& target) = 0;
        virtual void stop() = 0;
        virtual CaptureStatus acquireFrame(CapturedFrame& frame, std::chrono::milliseconds timeout) noexcept = 0;
        virtual void releaseFrame() noexcept = 0;
    };

    std::unique_ptr<CaptureSource> createCaptureSource();

}
