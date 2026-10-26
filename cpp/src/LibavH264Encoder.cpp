#include "LibavH264Encoder.hpp"

#include "H264Bitstream.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
}

#include <array>
#include <cerrno>
#include <cstdarg>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

namespace fm {
    namespace {
        constexpr std::int64_t BITS_PER_KILOBIT = 1000;
        constexpr std::uint32_t MAX_FRAMES_PER_SECOND = 1000;
        constexpr int ENDLESS_GOP = std::numeric_limits<int>::max();
        constexpr std::uint32_t MAX_STRIDE = std::numeric_limits<int>::max();
        constexpr int LOG_LINE_SIZE = 1024;

        std::mutex libavErrorMutex;
        std::string libavErrors;
        bool libavLineEnded = true;

        std::string libavError(int code) {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
            ::av_strerror(code, text.data(), text.size());
            return text.data();
        }

        void collectLibavError(void* object, int level, const char* format, std::va_list arguments) {
            if (level > AV_LOG_ERROR) {
                return;
            }
            std::array<char, LOG_LINE_SIZE> line{};
            int printPrefix = 0;
            ::av_log_format_line2(object, level, format, arguments, line.data(), LOG_LINE_SIZE, &printPrefix);
            std::string_view text(line.data());
            const bool endsLine = text.ends_with('\n');
            if (endsLine) {
                text.remove_suffix(1);
            }
            const std::lock_guard lock(libavErrorMutex);
            if (libavLineEnded && !libavErrors.empty() && !text.empty()) {
                libavErrors += "; ";
            }
            libavErrors += text;
            libavLineEnded = endsLine;
        }

        class LibavErrorCollector {
        public:
            LibavErrorCollector() {
                {
                    const std::lock_guard lock(libavErrorMutex);
                    libavErrors.clear();
                    libavLineEnded = true;
                }
                ::av_log_set_callback(collectLibavError);
            }

            ~LibavErrorCollector() {
                ::av_log_set_callback(::av_log_default_callback);
            }

            LibavErrorCollector(const LibavErrorCollector&) = delete;
            LibavErrorCollector& operator=(const LibavErrorCollector&) = delete;

            std::string collected() const {
                const std::lock_guard lock(libavErrorMutex);
                return libavErrors;
            }
        };

        AVPixelFormat toSoftwareFormat(PixelFormat format) {
            switch (format) {
                case PixelFormat::BGRA8:
                    return AV_PIX_FMT_BGR0;
                case PixelFormat::RGBA8:
                    return AV_PIX_FMT_RGB0;
            }
            return AV_PIX_FMT_NONE;
        }
    }

    void LibavH264Encoder::ContextDeleter::operator()(AVCodecContext* context) const noexcept {
        ::avcodec_free_context(&context);
    }

    void LibavH264Encoder::FrameDeleter::operator()(AVFrame* frame) const noexcept {
        ::av_frame_free(&frame);
    }

    void LibavH264Encoder::PacketDeleter::operator()(AVPacket* packet) const noexcept {
        ::av_packet_free(&packet);
    }

    LibavH264Encoder::LibavH264Encoder(std::string codecName, std::string description, std::size_t maxFrameSize)
        : codecName(std::move(codecName)), encoderDescription(std::move(description)), maxFrameSize(maxFrameSize) {}

    Result<> LibavH264Encoder::configure(const EncoderConfig& config) {
        const LibavErrorCollector libavErrorCollector;
        auto configuredCodec = configureCodec(config);
        if (const std::string reason = libavErrorCollector.collected(); !configuredCodec && !reason.empty()) {
            configuredCodec.error().message += " (FFmpeg: " + reason + ")";
        }
        return configuredCodec;
    }

    Result<> LibavH264Encoder::configureCodec(const EncoderConfig& config) {
        configured = false;
        if (config.width < 2 || config.height < 2) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "H.264 needs a frame of at least 2x2 pixels"});
        }
        if (config.framesPerSecond == 0 || config.framesPerSecond > MAX_FRAMES_PER_SECOND) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "H.264 frame rate must be 1 to 1000"});
        }
        if (config.bitrateKbps == 0) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Encoder bitrate must not be zero"});
        }
        const AVPixelFormat softwareFormat = toSoftwareFormat(config.inputFormat);
        if (softwareFormat == AV_PIX_FMT_NONE) {
            return std::unexpected(
                    Error{ErrorCode::NOT_SUPPORTED, "Pixel format is not supported by the H.264 encoder"});
        }
        const AVCodec* codec = ::avcodec_find_encoder_by_name(codecName.c_str());
        if (codec == nullptr) {
            return std::unexpected(Error{ErrorCode::NOT_SUPPORTED, codecName + " is not in this FFmpeg build"});
        }

        ::av_log_set_level(AV_LOG_ERROR);
        context.reset(::avcodec_alloc_context3(codec));
        inputView.reset(::av_frame_alloc());
        packet.reset(::av_packet_alloc());
        if (!context || !inputView || !packet) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not allocate the H.264 encoder"});
        }

        encodedWidth = config.width & ~1U;
        encodedHeight = config.height & ~1U;
        const auto framesPerSecond = static_cast<int>(config.framesPerSecond);
        context->width = static_cast<int>(encodedWidth);
        context->height = static_cast<int>(encodedHeight);
        context->pix_fmt = softwareFormat;
        context->time_base = AVRational{1, framesPerSecond};
        context->framerate = AVRational{framesPerSecond, 1};
        context->bit_rate = static_cast<std::int64_t>(config.bitrateKbps) * BITS_PER_KILOBIT;
        context->rc_max_rate = context->bit_rate;
        context->rc_buffer_size = static_cast<int>(context->bit_rate / framesPerSecond);
        context->max_b_frames = 0;
        context->gop_size = ENDLESS_GOP;

        AVDictionary* options = nullptr;
        if (auto prepared = prepareContext(*context, options); !prepared) {
            ::av_dict_free(&options);
            return prepared;
        }
        const int opened = ::avcodec_open2(context.get(), codec, &options);
        const AVDictionaryEntry* unknownOption = ::av_dict_iterate(options, nullptr);
        const std::string unknownName = unknownOption != nullptr ? unknownOption->key : "";
        ::av_dict_free(&options);
        if (opened < 0) {
            return std::unexpected(
                    Error{ErrorCode::DEVICE_FAILURE, "Could not open " + codecName + ": " + libavError(opened)});
        }
        if (!unknownName.empty()) {
            context.reset(::avcodec_alloc_context3(codec));
            return std::unexpected(
                    Error{ErrorCode::NOT_SUPPORTED, codecName + " does not know the option " + unknownName});
        }

        inputView->format = softwareFormat;
        inputView->width = static_cast<int>(encodedWidth);
        inputView->height = static_cast<int>(encodedHeight);

        this->config = config;
        nextPresentationTime = 0;
        keyframeRequested = true;
        configured = true;
        return {};
    }

    EncodeStatus LibavH264Encoder::encode(const CapturedFrame& input, EncodedFrame& output) noexcept {
        const std::size_t rowBytes = static_cast<std::size_t>(config.width) * bytesPerPixel(config.inputFormat);
        if (!configured || input.pixels == nullptr || input.width != config.width || input.height != config.height ||
            input.format != config.inputFormat || input.stride < rowBytes || input.stride > MAX_STRIDE) {
            return EncodeStatus::ENCODE_FAILED;
        }

        ::av_packet_unref(packet.get());
        inputView->data[0] = const_cast<std::uint8_t*>(input.pixels);
        inputView->linesize[0] = static_cast<int>(input.stride);
        AVFrame* prepared = prepareFrame(*inputView);
        if (prepared == nullptr) {
            return EncodeStatus::ENCODE_FAILED;
        }

        const bool keyframe = std::exchange(keyframeRequested, false);
        prepared->pts = nextPresentationTime++;
        prepared->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (::avcodec_send_frame(context.get(), prepared) < 0) {
            if (keyframe) {
                keyframeRequested = true;
            }
            return EncodeStatus::ENCODE_FAILED;
        }

        const int received = ::avcodec_receive_packet(context.get(), packet.get());
        if (received == AVERROR(EAGAIN)) {
            return EncodeStatus::NO_OUTPUT;
        }
        if (received >= 0 && packet->size > 0 && ::av_packet_make_writable(packet.get()) >= 0) {
            const std::span<std::uint8_t> accessUnit(packet->data, static_cast<std::size_t>(packet->size));
            ::av_shrink_packet(packet.get(), static_cast<int>(removeFillerData(accessUnit)));
        } else {
            ::av_packet_unref(packet.get());
        }
        if (packet->size <= 0 || static_cast<std::size_t>(packet->size) > maxFrameSize) {
            ::av_packet_unref(packet.get());
            keyframeRequested = true;
            return EncodeStatus::ENCODE_FAILED;
        }

        output.data = std::span<const std::uint8_t>(packet->data, static_cast<std::size_t>(packet->size));
        output.codec = Codec::H264;
        output.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        output.captureTime = input.captureTime;
        return EncodeStatus::FRAME_ENCODED;
    }

    void LibavH264Encoder::requestKeyframe() noexcept {
        keyframeRequested = true;
    }

    std::string_view LibavH264Encoder::description() const noexcept {
        return encoderDescription;
    }

}
