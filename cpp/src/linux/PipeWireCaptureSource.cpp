#include "linux/PipeWireCaptureSource.hpp"

#include "linux/PipeWireFormat.hpp"
#include "linux/RestoreTokenStore.hpp"

#include <pipewire/pipewire.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>

namespace fm {
    struct PipeWireStream {
        FrameMailbox<CapturedFrame>& mailbox;

        pw_thread_loop* threadLoop = nullptr;
        pw_context* context = nullptr;
        pw_core* core = nullptr;
        pw_stream* pipeWireStream = nullptr;
        spa_hook listener{};

        pw_stream_state state = PW_STREAM_STATE_UNCONNECTED;
        std::string errorMessage;

        PixelFormat format = PixelFormat::BGRA8;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        bool formatKnown = false;
        bool reserved = false;
        bool pipeWireInitialized = false;

        explicit PipeWireStream(FrameMailbox<CapturedFrame>& mailbox) : mailbox(mailbox) {}
    };

    namespace {
        constexpr std::chrono::seconds STREAM_START_TIMEOUT{5};
        constexpr std::chrono::seconds MAX_TIMESTAMP_AGE{1};

        class ThreadLoopLock {
        private:
            pw_thread_loop* threadLoop;

        public:
            explicit ThreadLoopLock(pw_thread_loop* threadLoop) : threadLoop(threadLoop) {
                ::pw_thread_loop_lock(threadLoop);
            }

            ~ThreadLoopLock() {
                ::pw_thread_loop_unlock(threadLoop);
            }

            ThreadLoopLock(const ThreadLoopLock&) = delete;
            ThreadLoopLock& operator=(const ThreadLoopLock&) = delete;
        };

        std::chrono::steady_clock::time_point captureTimeOf(const spa_buffer& buffer) {
            const auto now = std::chrono::steady_clock::now();
            const auto* header = static_cast<const spa_meta_header*>(
                    ::spa_buffer_find_meta_data(&buffer, SPA_META_Header, sizeof(spa_meta_header)));
            if (header == nullptr || header->pts <= 0) {
                return now;
            }
            const std::chrono::steady_clock::time_point presented{
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::nanoseconds(header->pts))};
            if (presented > now || now - presented > MAX_TIMESTAMP_AGE) {
                return now;
            }
            return presented;
        }

        void copyFrame(PipeWireStream& stream, const spa_buffer& buffer) {
            if (!stream.formatKnown || buffer.n_datas < 1) {
                return;
            }
            const spa_data& data = buffer.datas[0];
            if (data.data == nullptr || data.chunk == nullptr) {
                return;
            }
            const spa_chunk& chunk = *data.chunk;
            if ((static_cast<std::uint32_t>(chunk.flags) & SPA_CHUNK_FLAG_CORRUPTED) != 0 || chunk.size == 0) {
                return;
            }
            const std::uint32_t rowBytes = stream.width * bytesPerPixel(stream.format);
            const std::uint32_t stride = chunk.stride > 0 ? static_cast<std::uint32_t>(chunk.stride) : rowBytes;
            if (stride < rowBytes) {
                return;
            }
            const std::size_t frameBytes = static_cast<std::size_t>(stride) * stream.height;
            if (chunk.offset > data.maxsize || frameBytes > data.maxsize - chunk.offset) {
                return;
            }

            std::uint8_t* target = stream.mailbox.beginWrite(frameBytes);
            std::memcpy(target, static_cast<const std::uint8_t*>(data.data) + chunk.offset, frameBytes);

            CapturedFrame description;
            description.width = stream.width;
            description.height = stream.height;
            description.stride = stride;
            description.format = stream.format;
            description.captureTime = captureTimeOf(buffer);
            stream.mailbox.publish(description, frameBytes);
        }

        void onStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error) {
            auto* stream = static_cast<PipeWireStream*>(data);
            stream->state = state;
            if (state == PW_STREAM_STATE_ERROR) {
                stream->errorMessage = error != nullptr ? error : "PipeWire stream error";
            }
            if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
                stream->mailbox.close();
            }
            ::pw_thread_loop_signal(stream->threadLoop, false);
        }

        void onParamChanged(void* data, std::uint32_t id, const spa_pod* param) {
            auto* stream = static_cast<PipeWireStream*>(data);
            if (param == nullptr || id != SPA_PARAM_Format) {
                return;
            }
            std::uint32_t mediaType = 0;
            std::uint32_t mediaSubtype = 0;
            if (::spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video ||
                mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
                return;
            }
            spa_video_info_raw info{};
            if (::spa_format_video_raw_parse(param, &info) < 0) {
                return;
            }
            const auto pixelFormat = toPixelFormat(info.format);
            if (!pixelFormat || info.size.width == 0 || info.size.height == 0) {
                ::pw_stream_set_error(stream->pipeWireStream, -EINVAL, "unsupported video format");
                return;
            }
            stream->format = *pixelFormat;
            stream->width = info.size.width;
            stream->height = info.size.height;
            stream->formatKnown = true;
            if (!stream->reserved) {
                stream->mailbox.reserve(static_cast<std::size_t>(stream->width) * stream->height *
                                        bytesPerPixel(stream->format));
                stream->reserved = true;
            }

            std::uint8_t podBuffer[POD_BUFFER_SIZE];
            spa_pod_builder builder{};
            ::spa_pod_builder_init(&builder, podBuffer, sizeof(podBuffer));
            const spa_pod* params[] = {buildBufferParameter(builder), buildTimestampParameter(builder)};
            if (params[0] == nullptr || params[1] == nullptr) {
                ::pw_stream_set_error(stream->pipeWireStream, -ENOSPC, "buffer parameters do not fit");
                return;
            }
            ::pw_stream_update_params(stream->pipeWireStream, params, 2);
        }

        void onProcess(void* data) {
            auto* stream = static_cast<PipeWireStream*>(data);
            pw_buffer* newest = nullptr;
            while (pw_buffer* buffer = ::pw_stream_dequeue_buffer(stream->pipeWireStream)) {
                if (newest != nullptr) {
                    ::pw_stream_queue_buffer(stream->pipeWireStream, newest);
                }
                newest = buffer;
            }
            if (newest == nullptr) {
                return;
            }
            copyFrame(*stream, *newest->buffer);
            ::pw_stream_queue_buffer(stream->pipeWireStream, newest);
        }

        const pw_stream_events STREAM_EVENTS = [] {
            pw_stream_events events{};
            events.version = PW_VERSION_STREAM_EVENTS;
            events.state_changed = onStateChanged;
            events.param_changed = onParamChanged;
            events.process = onProcess;
            return events;
        }();

    }

    PipeWireCaptureSource::PipeWireCaptureSource() = default;

    PipeWireCaptureSource::~PipeWireCaptureSource() {
        stop();
    }

    Result<> PipeWireCaptureSource::start(const CaptureTarget& target) {
        if (started) {
            return std::unexpected(Error{ErrorCode::INVALID_STATE, "Capture is already started"});
        }
        mailbox.reset();

        std::optional<RestoreTokenStore> tokenStore;
        if (auto tokenPath = RestoreTokenStore::defaultPath(target.outputName)) {
            tokenStore.emplace(*tokenPath);
        }
        const std::optional<std::string> restoreToken = tokenStore ? tokenStore->load() : std::nullopt;

        portal = std::make_unique<PortalScreenCast>();
        auto screenCast = portal->start(restoreToken);
        if (!screenCast) {
            portal.reset();
            return std::unexpected(screenCast.error());
        }
        if (target.width != 0 && screenCast->width != 0 &&
            (screenCast->width != target.width || screenCast->height != target.height)) {
            ::close(screenCast->pipeWireDescriptor);
            portal.reset();
            return std::unexpected(Error{
                    ErrorCode::INVALID_STATE,
                    std::format("The screen sharing dialog shared a {}x{} monitor, but {} is {}x{}. Start again and "
                                "share {}.",
                                screenCast->width, screenCast->height, target.outputName, target.width, target.height,
                                target.outputName)});
        }
        if (tokenStore && screenCast->restoreToken) {
            static_cast<void>(tokenStore->save(*screenCast->restoreToken));
        }

        if (auto connected = connectStream(screenCast->pipeWireDescriptor, screenCast->nodeId); !connected) {
            disconnectStream();
            portal.reset();
            return std::unexpected(connected.error());
        }
        started = true;
        return {};
    }

    Result<> PipeWireCaptureSource::connectStream(int pipeWireDescriptor, std::uint32_t nodeId) {
        stream = std::make_unique<PipeWireStream>(mailbox);
        ::pw_init(nullptr, nullptr);
        stream->pipeWireInitialized = true;

        stream->threadLoop = ::pw_thread_loop_new("fm-capture", nullptr);
        if (stream->threadLoop == nullptr) {
            ::close(pipeWireDescriptor);
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot create the PipeWire thread loop"});
        }
        stream->context = ::pw_context_new(::pw_thread_loop_get_loop(stream->threadLoop), nullptr, 0);
        if (stream->context == nullptr) {
            ::close(pipeWireDescriptor);
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot create the PipeWire context"});
        }
        if (::pw_thread_loop_start(stream->threadLoop) < 0) {
            ::close(pipeWireDescriptor);
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot start the PipeWire thread loop"});
        }

        const ThreadLoopLock lock(stream->threadLoop);
        stream->core = ::pw_context_connect_fd(stream->context, pipeWireDescriptor, nullptr, 0);
        if (stream->core == nullptr) {
            ::close(pipeWireDescriptor);
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot connect to PipeWire"});
        }
        stream->pipeWireStream = ::pw_stream_new(stream->core, "FreeMonitor capture",
                                                 ::pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                                                     "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        if (stream->pipeWireStream == nullptr) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot create the PipeWire stream"});
        }
        ::pw_stream_add_listener(stream->pipeWireStream, &stream->listener, &STREAM_EVENTS, stream.get());

        std::uint8_t podBuffer[POD_BUFFER_SIZE];
        spa_pod_builder builder{};
        ::spa_pod_builder_init(&builder, podBuffer, sizeof(podBuffer));
        const spa_pod* params[] = {buildFormatParameter(builder, MAX_FRAMES_PER_SECOND)};
        if (params[0] == nullptr) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "The PipeWire format offer does not fit"});
        }

        const int connected = ::pw_stream_connect(
                stream->pipeWireStream, PW_DIRECTION_INPUT, nodeId,
                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
        if (connected < 0) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot connect the PipeWire stream"});
        }
        stream->state = PW_STREAM_STATE_CONNECTING;

        timespec deadline{};
        ::pw_thread_loop_get_time(stream->threadLoop, &deadline,
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(STREAM_START_TIMEOUT).count());
        while (stream->state == PW_STREAM_STATE_CONNECTING) {
            if (::pw_thread_loop_timed_wait_full(stream->threadLoop, &deadline) == -ETIMEDOUT) {
                break;
            }
        }
        if (stream->state != PW_STREAM_STATE_PAUSED && stream->state != PW_STREAM_STATE_STREAMING) {
            const std::string reason = stream->errorMessage.empty() ? "it did not start in time" : stream->errorMessage;
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "PipeWire stream failed: " + reason});
        }
        return {};
    }

    void PipeWireCaptureSource::disconnectStream() {
        if (!stream) {
            return;
        }
        if (stream->threadLoop != nullptr) {
            {
                const ThreadLoopLock lock(stream->threadLoop);
                if (stream->pipeWireStream != nullptr) {
                    ::pw_stream_disconnect(stream->pipeWireStream);
                    ::pw_stream_destroy(stream->pipeWireStream);
                    stream->pipeWireStream = nullptr;
                }
                if (stream->core != nullptr) {
                    ::pw_core_disconnect(stream->core);
                    stream->core = nullptr;
                }
            }
            ::pw_thread_loop_stop(stream->threadLoop);
        }
        if (stream->context != nullptr) {
            ::pw_context_destroy(stream->context);
        }
        if (stream->threadLoop != nullptr) {
            ::pw_thread_loop_destroy(stream->threadLoop);
        }
        if (stream->pipeWireInitialized) {
            ::pw_deinit();
        }
        stream.reset();
    }

    void PipeWireCaptureSource::stop() {
        mailbox.close();
        disconnectStream();
        portal.reset();
        started = false;
    }

    CaptureStatus PipeWireCaptureSource::acquireFrame(CapturedFrame& frame,
                                                      std::chrono::milliseconds timeout) noexcept {
        if (!started) {
            return CaptureStatus::CAPTURE_LOST;
        }
        std::span<const std::uint8_t> pixels;
        switch (mailbox.acquire(frame, pixels, timeout)) {
            case MailboxStatus::FRAME_READY:
                frame.pixels = pixels.data();
                return CaptureStatus::FRAME_READY;
            case MailboxStatus::NO_NEW_FRAME:
                return CaptureStatus::NO_NEW_FRAME;
            case MailboxStatus::CLOSED:
                return CaptureStatus::CAPTURE_LOST;
        }
        return CaptureStatus::CAPTURE_LOST;
    }

    void PipeWireCaptureSource::releaseFrame() noexcept {
        mailbox.release();
    }
}
