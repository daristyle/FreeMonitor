#pragma once

#include "VirtualDisplay.hpp"

#include <string_view>

namespace fm {
    class StubVirtualDisplay final : public VirtualDisplay {
    private:
        DisplayMode mode;
        bool created = false;

    public:
        static constexpr std::string_view OUTPUT_NAME = "STUB-1";

        StubVirtualDisplay() = default;
        ~StubVirtualDisplay() override;

        Result<CaptureTarget> create(const DisplayMode& mode) override;
        void destroy() override;

        bool exists() const {
            return created;
        }

        const DisplayMode& currentMode() const {
            return mode;
        }
    };

}
