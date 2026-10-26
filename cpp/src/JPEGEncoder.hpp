#pragma once

#include "Encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fm {
    class JPEGEncoder final : public Encoder {
    private:
        struct HandleDeleter {
            void operator()(void* handle) const noexcept;
        };

        static constexpr std::uint32_t MAX_JPEG_DIMENSION = 65500;
        static constexpr int MIN_JPEG_QUALITY = 10;
        static constexpr int JPEG_QUALITY_STEP = 20;

        std::size_t maxFrameSize;
        std::unique_ptr<void, HandleDeleter> handle;
        EncoderConfig config;
        int pixelFormat = 0;
        std::vector<std::uint8_t> outputBuffer;
        bool configured = false;

        bool compress(const CapturedFrame& input, int quality, std::size_t& jpegSize) noexcept;

    public:
        explicit JPEGEncoder(std::size_t maxFrameSize = MAX_FRAME_SIZE);

        Result<> configure(const EncoderConfig& config) override;
        EncodeStatus encode(const CapturedFrame& input, EncodedFrame& output) noexcept override;
        void requestKeyframe() noexcept override;
        std::string_view description() const noexcept override;
    };

}
