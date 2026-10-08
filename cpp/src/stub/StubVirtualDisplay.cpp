#include "stub/StubVirtualDisplay.hpp"

#include <string>

namespace fm {
    StubVirtualDisplay::~StubVirtualDisplay() {
        destroy();
    }

    Result<CaptureTarget> StubVirtualDisplay::create(const DisplayMode& mode) {
        if (created) {
            return std::unexpected(Error{ErrorCode::INVALID_STATE, "Virtual display already exists"});
        }
        if (mode.width == 0 || mode.height == 0 || mode.refreshRateHz == 0) {
            return std::unexpected(
                    Error{ErrorCode::INVALID_ARGUMENT, "Display size and refresh rate must not be zero"});
        }

        this->mode = mode;
        created = true;
        return CaptureTarget{std::string(OUTPUT_NAME)};
    }

    void StubVirtualDisplay::destroy() {
        created = false;
    }

}
