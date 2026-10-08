#pragma once

#include "CaptureSource.hpp"
#include "Error.hpp"

#include <cstdint>
#include <memory>

namespace fm {
    struct DisplayMode {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t refreshRateHz = 60;
        std::int32_t positionX = 0;
        std::int32_t positionY = 0;
    };

    class VirtualDisplay {
    protected:
        VirtualDisplay() = default;

    public:
        virtual ~VirtualDisplay() = default;

        VirtualDisplay(const VirtualDisplay&) = delete;
        VirtualDisplay& operator=(const VirtualDisplay&) = delete;

        virtual Result<CaptureTarget> create(const DisplayMode& mode) = 0;
        virtual void destroy() = 0;
    };

    std::unique_ptr<VirtualDisplay> createVirtualDisplay();

}
