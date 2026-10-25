#pragma once

#include "CaptureSource.hpp"
#include "FrameMailbox.hpp"
#include "linux/PortalScreenCast.hpp"

#include <cstdint>
#include <memory>

namespace fm {
    struct PipeWireStream;

    class PipeWireCaptureSource final : public CaptureSource {
    private:
        FrameMailbox<CapturedFrame> mailbox;
        std::unique_ptr<PortalScreenCast> portal;
        std::unique_ptr<PipeWireStream> stream;
        bool started = false;

        Result<> connectStream(int pipeWireDescriptor, std::uint32_t nodeId);
        void disconnectStream();

    public:
        static constexpr std::uint32_t MAX_FRAMES_PER_SECOND = 60;

        PipeWireCaptureSource();
        ~PipeWireCaptureSource() override;

        Result<> start(const CaptureTarget& target) override;
        void stop() override;
        CaptureStatus acquireFrame(CapturedFrame& frame, std::chrono::milliseconds timeout) noexcept override;
        void releaseFrame() noexcept override;
    };
}
