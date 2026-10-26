#include "NVENCEncoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <array>
#include <utility>

namespace fm {
    namespace {
        constexpr std::array<std::pair<const char*, const char*>, 7> NVENC_OPTIONS{{
                {"preset", "p1"},
                {"tune", "ull"},
                {"rc", "vbr"},
                {"zerolatency", "1"},
                {"delay", "0"},
                {"forced-idr", "1"},
                {"no-scenecut", "1"},
        }};
    }

    void NVENCEncoder::BufferDeleter::operator()(AVBufferRef* buffer) const noexcept {
        ::av_buffer_unref(&buffer);
    }

    void NVENCEncoder::FrameDeleter::operator()(AVFrame* frame) const noexcept {
        ::av_frame_free(&frame);
    }

    NVENCEncoder::NVENCEncoder(std::size_t maxFrameSize)
        : LibavH264Encoder("h264_nvenc", "H.264 NVENC", maxFrameSize) {}

    Result<> NVENCEncoder::prepareContext(AVCodecContext& context, AVDictionary*& options) {
        AVBufferRef* createdDevice = nullptr;
        if (::av_hwdevice_ctx_create(&createdDevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not open the CUDA device"});
        }
        const std::unique_ptr<AVBufferRef, BufferDeleter> device(createdDevice);
        frames.reset(::av_hwframe_ctx_alloc(device.get()));
        uploadFrame.reset(::av_frame_alloc());
        if (!frames || !uploadFrame) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not allocate the CUDA frames"});
        }

        auto& framesContext = *reinterpret_cast<AVHWFramesContext*>(frames->data);
        framesContext.format = AV_PIX_FMT_CUDA;
        framesContext.sw_format = context.pix_fmt;
        framesContext.width = context.width;
        framesContext.height = context.height;
        if (::av_hwframe_ctx_init(frames.get()) < 0) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not set up the CUDA frames"});
        }
        context.pix_fmt = AV_PIX_FMT_CUDA;
        context.hw_frames_ctx = ::av_buffer_ref(frames.get());
        if (context.hw_frames_ctx == nullptr) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not allocate the CUDA frames"});
        }

        for (const auto& [key, value] : NVENC_OPTIONS) {
            if (::av_dict_set(&options, key, value, 0) < 0) {
                return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not set the NVENC options"});
            }
        }
        return {};
    }

    AVFrame* NVENCEncoder::prepareFrame(AVFrame& softwareFrame) noexcept {
        ::av_frame_unref(uploadFrame.get());
        if (::av_hwframe_get_buffer(frames.get(), uploadFrame.get(), 0) < 0 ||
            ::av_hwframe_transfer_data(uploadFrame.get(), &softwareFrame, 0) < 0) {
            return nullptr;
        }
        return uploadFrame.get();
    }

}
