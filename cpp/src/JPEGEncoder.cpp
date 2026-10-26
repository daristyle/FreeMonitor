#include "JPEGEncoder.hpp"

#include <turbojpeg.h>

#include <algorithm>
#include <climits>
#include <string>

namespace fm {
    namespace {
        constexpr int toTurboJPEGFormat(PixelFormat format) {
            switch (format) {
                case PixelFormat::BGRA8:
                    return TJPF_BGRX;
                case PixelFormat::RGBA8:
                    return TJPF_RGBX;
            }
            return TJPF_UNKNOWN;
        }
    }

    void JPEGEncoder::HandleDeleter::operator()(void* handle) const noexcept {
        ::tj3Destroy(handle);
    }

    JPEGEncoder::JPEGEncoder(std::size_t maxFrameSize) : maxFrameSize(maxFrameSize) {}

    Result<> JPEGEncoder::configure(const EncoderConfig& config) {
        if (config.width == 0 || config.height == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Encoder size must not be zero"});
        }
        if (config.width > MAX_JPEG_DIMENSION || config.height > MAX_JPEG_DIMENSION) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "JPEG width and height must be at most 65500"});
        }
        if (config.framesPerSecond == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Encoder frame rate must not be zero"});
        }
        if (config.jpegQuality < 1 || config.jpegQuality > 100) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "JPEG quality must be 1 to 100"});
        }
        const int turboJPEGFormat = toTurboJPEGFormat(config.inputFormat);
        if (turboJPEGFormat == TJPF_UNKNOWN) {
            return std::unexpected(
                    Error{ErrorCode::NOT_SUPPORTED, "Pixel format is not supported by the JPEG encoder"});
        }

        if (!handle) {
            handle.reset(::tj3Init(TJINIT_COMPRESS));
            if (!handle) {
                return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not create the JPEG compressor"});
            }
        }
        if (::tj3Set(handle.get(), TJPARAM_SUBSAMP, TJSAMP_420) != 0 ||
            ::tj3Set(handle.get(), TJPARAM_NOREALLOC, 1) != 0) {
            return std::unexpected(
                    Error{ErrorCode::DEVICE_FAILURE,
                          std::string("Could not set up the JPEG compressor: ") + ::tj3GetErrorStr(handle.get())});
        }

        const std::size_t bufferSize =
                ::tj3JPEGBufSize(static_cast<int>(config.width), static_cast<int>(config.height), TJSAMP_420);
        if (bufferSize == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Encoder size is too large"});
        }

        outputBuffer.resize(bufferSize);
        this->config = config;
        pixelFormat = turboJPEGFormat;
        configured = true;
        return {};
    }

    EncodeStatus JPEGEncoder::encode(const CapturedFrame& input, EncodedFrame& output) noexcept {
        const std::size_t rowBytes = static_cast<std::size_t>(config.width) * bytesPerPixel(config.inputFormat);
        if (!configured || input.pixels == nullptr || input.width != config.width || input.height != config.height ||
            input.format != config.inputFormat || input.stride < rowBytes || input.stride > INT_MAX) {
            return EncodeStatus::ENCODE_FAILED;
        }

        int quality = config.jpegQuality;
        std::size_t jpegSize = 0;
        while (true) {
            if (!compress(input, quality, jpegSize)) {
                return EncodeStatus::ENCODE_FAILED;
            }
            if (jpegSize <= maxFrameSize) {
                break;
            }
            if (quality <= MIN_JPEG_QUALITY) {
                return EncodeStatus::ENCODE_FAILED;
            }
            quality = std::max(MIN_JPEG_QUALITY, quality - JPEG_QUALITY_STEP);
        }

        output.data = std::span<const std::uint8_t>(outputBuffer).first(jpegSize);
        output.codec = Codec::JPEG;
        output.keyframe = true;
        output.captureTime = input.captureTime;
        return EncodeStatus::FRAME_ENCODED;
    }

    void JPEGEncoder::requestKeyframe() noexcept {}

    std::string_view JPEGEncoder::description() const noexcept {
        return "JPEG";
    }

    bool JPEGEncoder::compress(const CapturedFrame& input, int quality, std::size_t& jpegSize) noexcept {
        if (::tj3Set(handle.get(), TJPARAM_QUALITY, quality) != 0) {
            return false;
        }
        unsigned char* jpegBuffer = outputBuffer.data();
        jpegSize = outputBuffer.size();
        return ::tj3Compress8(handle.get(), input.pixels, static_cast<int>(input.width), static_cast<int>(input.stride),
                              static_cast<int>(input.height), pixelFormat, &jpegBuffer, &jpegSize) == 0;
    }

}
