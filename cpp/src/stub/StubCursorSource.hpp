#pragma once

#include "CursorSource.hpp"

#include <chrono>

namespace fm {
    class StubCursorSource final : public CursorSource {
    private:
        CursorImage emptyImage;

    public:
        Result<> start(const CaptureTarget& target) override;
        void stop() override;
        CursorStatus waitForChange(CursorState& state, std::chrono::milliseconds timeout) noexcept override;
        void wake() noexcept override;
        const CursorImage& image() const noexcept override;
    };
}
