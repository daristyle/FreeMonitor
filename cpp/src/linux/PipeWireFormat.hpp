#pragma once

#include "Frame.hpp"

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include <cstdint>
#include <optional>

namespace fm {
    constexpr std::uint32_t POD_BUFFER_SIZE = 1024;
    constexpr std::uint32_t MAX_CAPTURE_SIZE = 8192;

    constexpr std::optional<PixelFormat> toPixelFormat(spa_video_format format) {
        switch (format) {
            case SPA_VIDEO_FORMAT_BGRx:
            case SPA_VIDEO_FORMAT_BGRA:
                return PixelFormat::BGRA8;
            case SPA_VIDEO_FORMAT_RGBx:
            case SPA_VIDEO_FORMAT_RGBA:
                return PixelFormat::RGBA8;
            default:
                return std::nullopt;
        }
    }

    inline const spa_pod* buildFormatParameter(spa_pod_builder& builder, std::uint32_t maxFramesPerSecond) {
        const spa_rectangle defaultSize{1920, 1080};
        const spa_rectangle minimumSize{1, 1};
        const spa_rectangle maximumSize{MAX_CAPTURE_SIZE, MAX_CAPTURE_SIZE};
        const spa_fraction variableFramerate{0, 1};
        const spa_fraction minimumFramerate{1, 1};
        const spa_fraction maximumFramerate{maxFramesPerSecond, 1};

        spa_pod_frame frame{};
        ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        ::spa_pod_builder_add(
                &builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
                SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
                SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                       SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
                SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minimumSize, &maximumSize),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variableFramerate), SPA_FORMAT_VIDEO_maxFramerate,
                SPA_POD_CHOICE_RANGE_Fraction(&maximumFramerate, &minimumFramerate, &maximumFramerate), 0);
        return static_cast<const spa_pod*>(::spa_pod_builder_pop(&builder, &frame));
    }

    inline const spa_pod* buildBufferParameter(spa_pod_builder& builder) {
        spa_pod_frame frame{};
        ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        ::spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_dataType,
                              SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)), 0);
        return static_cast<const spa_pod*>(::spa_pod_builder_pop(&builder, &frame));
    }

    inline const spa_pod* buildTimestampParameter(spa_pod_builder& builder) {
        spa_pod_frame frame{};
        ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
        ::spa_pod_builder_add(&builder, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
                              SPA_POD_Int(static_cast<int>(sizeof(spa_meta_header))), 0);
        return static_cast<const spa_pod*>(::spa_pod_builder_pop(&builder, &frame));
    }
}
