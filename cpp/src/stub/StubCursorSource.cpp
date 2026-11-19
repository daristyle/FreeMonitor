#include "stub/StubCursorSource.hpp"

namespace fm {
    Result<> StubCursorSource::start(const CaptureTarget&) {
        return std::unexpected(Error{ErrorCode::NOT_SUPPORTED, "This platform has no cursor source"});
    }

    void StubCursorSource::stop() {}

    CursorStatus StubCursorSource::waitForChange(CursorState&, std::chrono::milliseconds) noexcept {
        return CursorStatus::CURSOR_LOST;
    }

    void StubCursorSource::wake() noexcept {}

    const CursorImage& StubCursorSource::image() const noexcept {
        return emptyImage;
    }
}
