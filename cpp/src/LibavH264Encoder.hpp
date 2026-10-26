#pragma once

#include "Encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct AVCodecContext;
struct AVDictionary;
struct AVFrame;
struct AVPacket;

namespace fm {
    class LibavH264Encoder : public Encoder {
    private:
        struct ContextDeleter {
            void operator()(AVCodecContext* context) const noexcept;
        };

        struct FrameDeleter {
            void operator()(AVFrame* frame) const noexcept;
        };

        struct PacketDeleter {
            void operator()(AVPacket* packet) const noexcept;
        };

        std::string codecName;
        std::string encoderDescription;
        std::size_t maxFrameSize;
        std::unique_ptr<AVCodecContext, ContextDeleter> context;
        std::unique_ptr<AVFrame, FrameDeleter> inputView;
        std::unique_ptr<AVPacket, PacketDeleter> packet;
        EncoderConfig config;
        std::uint32_t encodedWidth = 0;
        std::uint32_t encodedHeight = 0;
        std::int64_t nextPresentationTime = 0;
        bool keyframeRequested = true;
        bool configured = false;

        Result<> configureCodec(const EncoderConfig& config);

    protected:
        LibavH264Encoder(std::string codecName, std::string description, std::size_t maxFrameSize);

        virtual Result<> prepareContext(AVCodecContext& context, AVDictionary*& options) = 0;
        virtual AVFrame* prepareFrame(AVFrame& softwareFrame) noexcept = 0;

    public:
        Result<> configure(const EncoderConfig& config) override;
        EncodeStatus encode(const CapturedFrame& input, EncodedFrame& output) noexcept override;
        void requestKeyframe() noexcept override;
        std::string_view description() const noexcept override;
    };

}
