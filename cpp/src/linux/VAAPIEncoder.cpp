#include "linux/VAAPIEncoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <array>
#include <string>
#include <utility>

namespace fm {
    namespace {
        constexpr std::array<std::pair<const char*, const char*>, 2> VAAPI_OPTIONS{{
                {"rc_mode", "CBR"},
                {"async_depth", "1"},
        }};

        std::string describe(const VAAPIDevice& device) {
            return device.path.empty() ? "H.264 VA-API" : "H.264 VA-API on " + device.path;
        }
    }

    void VAAPIEncoder::BufferDeleter::operator()(AVBufferRef* buffer) const noexcept {
        ::av_buffer_unref(&buffer);
    }

    void VAAPIEncoder::FrameDeleter::operator()(AVFrame* frame) const noexcept {
        ::av_frame_free(&frame);
    }

    VAAPIEncoder::VAAPIEncoder(VAAPIDevice device, std::size_t maxFrameSize)
        : LibavH264Encoder("h264_vaapi", describe(device), maxFrameSize), device(std::move(device)) {}

    Result<> VAAPIEncoder::prepareContext(AVCodecContext& context, AVDictionary*& options) {
        if (device.path.empty()) {
            return std::unexpected(Error{ErrorCode::NOT_SUPPORTED, "No AMD or Intel GPU with VA-API was found"});
        }
        AVDictionary* deviceOptions = nullptr;
        if (::av_dict_set(&deviceOptions, "driver", device.driver.c_str(), 0) < 0) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not set the VA-API driver"});
        }
        AVBufferRef* createdDevice = nullptr;
        const int created =
                ::av_hwdevice_ctx_create(&createdDevice, AV_HWDEVICE_TYPE_VAAPI, device.path.c_str(), deviceOptions, 0);
        ::av_dict_free(&deviceOptions);
        if (created < 0) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not open VA-API on " + device.path});
        }
        const std::unique_ptr<AVBufferRef, BufferDeleter> hardwareDevice(createdDevice);
        frames.reset(::av_hwframe_ctx_alloc(hardwareDevice.get()));
        uploadFrame.reset(::av_frame_alloc());
        if (!frames || !uploadFrame) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not allocate the VA-API frames"});
        }

        auto& framesContext = *reinterpret_cast<AVHWFramesContext*>(frames->data);
        framesContext.format = AV_PIX_FMT_VAAPI;
        framesContext.sw_format = AV_PIX_FMT_NV12;
        framesContext.width = context.width;
        framesContext.height = context.height;
        if (::av_hwframe_ctx_init(frames.get()) < 0) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not set up the VA-API frames"});
        }
        context.pix_fmt = AV_PIX_FMT_VAAPI;
        context.colorspace = AVCOL_SPC_BT709;
        context.color_primaries = AVCOL_PRI_BT709;
        context.color_trc = AVCOL_TRC_BT709;
        context.color_range = AVCOL_RANGE_MPEG;
        context.hw_frames_ctx = ::av_buffer_ref(frames.get());
        if (context.hw_frames_ctx == nullptr) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not allocate the VA-API frames"});
        }

        for (const auto& [key, value] : VAAPI_OPTIONS) {
            if (::av_dict_set(&options, key, value, 0) < 0) {
                return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not set the VA-API options"});
            }
        }
        return {};
    }

    AVFrame* VAAPIEncoder::prepareFrame(AVFrame& softwareFrame) noexcept {
        ::av_frame_unref(uploadFrame.get());
        if (::av_hwframe_get_buffer(frames.get(), uploadFrame.get(), 0) < 0 ||
            ::av_hwframe_transfer_data(uploadFrame.get(), &softwareFrame, 0) < 0) {
            return nullptr;
        }
        return uploadFrame.get();
    }

}
