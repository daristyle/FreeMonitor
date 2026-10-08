#include "stub/StubEncoder.hpp"

#include <cstddef>
#include <cstring>

namespace fm {
    StubEncoder::StubEncoder(Codec codec) : codec(codec) {}

    Result<> StubEncoder::configure(const EncoderConfig& config) {
        if (config.width == 0 || config.height == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Encoder size must not be zero"});
        }
        if (config.framesPerSecond == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Encoder frame rate must not be zero"});
        }
        if (codec == Codec::JPEG && (config.jpegQuality < 1 || config.jpegQuality > 100)) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "JPEG quality must be 1 to 100"});
        }

        this->config = config;
        outputBuffer.resize(static_cast<std::size_t>(config.width) * config.height * bytesPerPixel(config.inputFormat));
        configured = true;
        keyframeRequested.store(true, std::memory_order_relaxed);
        return {};
    }

    EncodeStatus StubEncoder::encode(const CapturedFrame& input, EncodedFrame& output) noexcept {
        const std::size_t rowBytes = static_cast<std::size_t>(config.width) * bytesPerPixel(config.inputFormat);
        if (!configured || input.pixels == nullptr || input.width != config.width || input.height != config.height ||
            input.format != config.inputFormat || input.stride < rowBytes) {
            return EncodeStatus::ENCODE_FAILED;
        }

        for (std::uint32_t y = 0; y < config.height; ++y) {
            std::memcpy(outputBuffer.data() + y * rowBytes, input.pixels + static_cast<std::size_t>(y) * input.stride,
                        rowBytes);
        }

        const bool requested = keyframeRequested.exchange(false, std::memory_order_relaxed);
        output.data = outputBuffer;
        output.keyframe = codec == Codec::JPEG || requested;
        output.captureTime = input.captureTime;
        return EncodeStatus::FRAME_ENCODED;
    }

    void StubEncoder::requestKeyframe() noexcept {
        keyframeRequested.store(true, std::memory_order_relaxed);
    }

}
