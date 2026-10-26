#pragma once

#include "LibavH264Encoder.hpp"

#include <cstddef>
#include <memory>

struct AVBufferRef;

namespace fm {
    class NVENCEncoder final : public LibavH264Encoder {
    private:
        struct BufferDeleter {
            void operator()(AVBufferRef* buffer) const noexcept;
        };

        struct FrameDeleter {
            void operator()(AVFrame* frame) const noexcept;
        };

        std::unique_ptr<AVBufferRef, BufferDeleter> frames;
        std::unique_ptr<AVFrame, FrameDeleter> uploadFrame;

        Result<> prepareContext(AVCodecContext& context, AVDictionary*& options) override;
        AVFrame* prepareFrame(AVFrame& softwareFrame) noexcept override;

    public:
        explicit NVENCEncoder(std::size_t maxFrameSize = MAX_FRAME_SIZE);
    };

}
