#pragma once

#include "Error.hpp"
#include "Frame.hpp"
#include "Packets.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace fm {
    enum class EncoderChoice : std::uint8_t {
        AUTO,
        NVENC,
        VAAPI,
        JPEG,
    };

    struct EncoderConfig {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        PixelFormat inputFormat = PixelFormat::BGRA8;
        std::uint32_t framesPerSecond = 60;
        std::uint32_t bitrateKbps = 20000;
        std::uint8_t jpegQuality = 80;
    };

    enum class EncodeStatus : std::uint8_t {
        FRAME_ENCODED,
        NO_OUTPUT,
        ENCODE_FAILED,
    };

    class Encoder {
    protected:
        Encoder() = default;

    public:
        virtual ~Encoder() = default;

        Encoder(const Encoder&) = delete;
        Encoder& operator=(const Encoder&) = delete;

        virtual Result<> configure(const EncoderConfig& config) = 0;
        virtual EncodeStatus encode(const CapturedFrame& input, EncodedFrame& output) noexcept = 0;
        virtual void requestKeyframe() noexcept = 0;
        virtual std::string_view description() const noexcept = 0;
    };

    std::unique_ptr<Encoder> createEncoder(EncoderChoice choice);

}
