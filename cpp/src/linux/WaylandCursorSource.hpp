#pragma once

#include "CursorSource.hpp"
#include "linux/FileDescriptor.hpp"

#include <chrono>
#include <memory>

namespace fm {
    struct WaylandCursorConnection;

    class WaylandCursorSource final : public CursorSource {
    private:
        FileDescriptor wakeEvent;
        std::unique_ptr<WaylandCursorConnection> connection;

    public:
        WaylandCursorSource();
        ~WaylandCursorSource() override;

        Result<> start(const CaptureTarget& target) override;
        void stop() override;
        CursorStatus waitForChange(CursorState& state, std::chrono::milliseconds timeout) noexcept override;
        void wake() noexcept override;
        const CursorImage& image() const noexcept override;
    };
}
