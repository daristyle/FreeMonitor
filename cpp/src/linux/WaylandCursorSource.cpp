#include "linux/WaylandCursorSource.hpp"

#include "CursorTracker.hpp"
#include "linux/XCursorTheme.hpp"

#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <wayland-client.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

namespace fm {
    struct WaylandCursorConnection {
        struct Output {
            WaylandCursorConnection* owner = nullptr;
            wl_output* output = nullptr;
            std::uint32_t globalName = 0;
            std::string name;
            std::int32_t scale = 1;
            std::int32_t pendingScale = 1;
        };

        wl_display* display = nullptr;
        wl_registry* registry = nullptr;
        wl_seat* seat = nullptr;
        std::uint32_t seatCapabilities = 0;
        wl_pointer* pointer = nullptr;
        ext_image_copy_capture_manager_v1* captureManager = nullptr;
        ext_output_image_capture_source_manager_v1* sourceManager = nullptr;
        ext_image_capture_source_v1* source = nullptr;
        ext_image_copy_capture_cursor_session_v1* cursorSession = nullptr;
        std::vector<std::unique_ptr<Output>> outputs;
        Output* target = nullptr;
        CursorTracker tracker;
        CursorTheme theme;
        std::vector<std::filesystem::path> searchPath;
        CursorImage image;
        std::int32_t imageScale = 0;
        std::uint32_t imageId = 0;
        bool lost = false;

        WaylandCursorConnection() = default;
        WaylandCursorConnection(const WaylandCursorConnection&) = delete;
        WaylandCursorConnection& operator=(const WaylandCursorConnection&) = delete;

        ~WaylandCursorConnection() {
            if (cursorSession != nullptr) {
                ::ext_image_copy_capture_cursor_session_v1_destroy(cursorSession);
            }
            if (source != nullptr) {
                ::ext_image_capture_source_v1_destroy(source);
            }
            if (pointer != nullptr) {
                ::wl_pointer_destroy(pointer);
            }
            for (const auto& output : outputs) {
                ::wl_output_release(output->output);
            }
            if (sourceManager != nullptr) {
                ::ext_output_image_capture_source_manager_v1_destroy(sourceManager);
            }
            if (captureManager != nullptr) {
                ::ext_image_copy_capture_manager_v1_destroy(captureManager);
            }
            if (seat != nullptr) {
                ::wl_seat_destroy(seat);
            }
            if (registry != nullptr) {
                ::wl_registry_destroy(registry);
            }
            if (display != nullptr) {
                ::wl_display_flush(display);
                ::wl_display_disconnect(display);
            }
        }

        void reloadImageForScale() {
            const std::int32_t scale = std::max(target->scale, 1);
            if (scale == imageScale) {
                return;
            }
            const std::uint32_t nominalSize =
                    std::min<std::uint32_t>(theme.size * static_cast<std::uint32_t>(scale), MAX_CURSOR_IMAGE_SIDE);
            image = loadArrowCursor(searchPath, theme.name, nominalSize);
            imageScale = scale;
            tracker.setImageId(++imageId);
        }
    };

    namespace {
        constexpr std::uint32_t SEAT_VERSION = 1;
        constexpr std::uint32_t OUTPUT_VERSION = 4;
        constexpr std::uint32_t CAPTURE_VERSION = 1;

        const CursorImage EMPTY_IMAGE{};

        WaylandCursorConnection& connectionOf(void* data) noexcept {
            return *static_cast<WaylandCursorConnection*>(data);
        }

        WaylandCursorConnection::Output& outputOf(void* data) noexcept {
            return *static_cast<WaylandCursorConnection::Output*>(data);
        }

        void onSeatCapabilities(void* data, wl_seat*, std::uint32_t capabilities) noexcept {
            connectionOf(data).seatCapabilities = capabilities;
        }

        void onSeatName(void*, wl_seat*, const char*) noexcept {}

        const wl_seat_listener SEAT_LISTENER{.capabilities = onSeatCapabilities, .name = onSeatName};

        void onOutputGeometry(void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                              const char*, const char*, std::int32_t) noexcept {}

        void onOutputMode(void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) noexcept {}

        void onOutputDone(void* data, wl_output*) noexcept {
            auto& output = outputOf(data);
            output.scale = output.pendingScale;
            if (output.owner->target == &output) {
                output.owner->reloadImageForScale();
            }
        }

        void onOutputScale(void* data, wl_output*, std::int32_t factor) noexcept {
            outputOf(data).pendingScale = factor;
        }

        void onOutputName(void* data, wl_output*, const char* name) noexcept {
            outputOf(data).name = name;
        }

        void onOutputDescription(void*, wl_output*, const char*) noexcept {}

        const wl_output_listener OUTPUT_LISTENER{.geometry = onOutputGeometry,
                                                 .mode = onOutputMode,
                                                 .done = onOutputDone,
                                                 .scale = onOutputScale,
                                                 .name = onOutputName,
                                                 .description = onOutputDescription};

        void onRegistryGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                              std::uint32_t version) noexcept {
            auto& connection = connectionOf(data);
            const std::string_view kind(interface);
            if (kind == wl_seat_interface.name && connection.seat == nullptr) {
                connection.seat =
                        static_cast<wl_seat*>(::wl_registry_bind(registry, name, &wl_seat_interface, SEAT_VERSION));
                ::wl_seat_add_listener(connection.seat, &SEAT_LISTENER, &connection);
            } else if (kind == wl_output_interface.name && version >= OUTPUT_VERSION) {
                auto output = std::make_unique<WaylandCursorConnection::Output>();
                output->owner = &connection;
                output->globalName = name;
                output->output = static_cast<wl_output*>(
                        ::wl_registry_bind(registry, name, &wl_output_interface, OUTPUT_VERSION));
                ::wl_output_add_listener(output->output, &OUTPUT_LISTENER, output.get());
                connection.outputs.push_back(std::move(output));
            } else if (kind == ext_image_copy_capture_manager_v1_interface.name &&
                       connection.captureManager == nullptr) {
                connection.captureManager = static_cast<ext_image_copy_capture_manager_v1*>(::wl_registry_bind(
                        registry, name, &ext_image_copy_capture_manager_v1_interface, CAPTURE_VERSION));
            } else if (kind == ext_output_image_capture_source_manager_v1_interface.name &&
                       connection.sourceManager == nullptr) {
                connection.sourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(::wl_registry_bind(
                        registry, name, &ext_output_image_capture_source_manager_v1_interface, CAPTURE_VERSION));
            }
        }

        void onRegistryGlobalRemove(void* data, wl_registry*, std::uint32_t name) noexcept {
            auto& connection = connectionOf(data);
            if (connection.target != nullptr && connection.target->globalName == name) {
                connection.lost = true;
            }
        }

        const wl_registry_listener REGISTRY_LISTENER{.global = onRegistryGlobal,
                                                     .global_remove = onRegistryGlobalRemove};

        void onCursorEnter(void* data, ext_image_copy_capture_cursor_session_v1*) noexcept {
            connectionOf(data).tracker.enter();
        }

        void onCursorLeave(void* data, ext_image_copy_capture_cursor_session_v1*) noexcept {
            connectionOf(data).tracker.leave();
        }

        void onCursorPosition(void* data, ext_image_copy_capture_cursor_session_v1*, std::int32_t x,
                              std::int32_t y) noexcept {
            connectionOf(data).tracker.moveTo(x, y);
        }

        void onCursorHotspot(void* data, ext_image_copy_capture_cursor_session_v1*, std::int32_t x,
                             std::int32_t y) noexcept {
            connectionOf(data).tracker.setHotspot(x, y);
        }

        const ext_image_copy_capture_cursor_session_v1_listener CURSOR_LISTENER{.enter = onCursorEnter,
                                                                                .leave = onCursorLeave,
                                                                                .position = onCursorPosition,
                                                                                .hotspot = onCursorHotspot};

        std::unexpected<Error> failure(ErrorCode code, std::string message) {
            return std::unexpected(Error{code, std::move(message)});
        }
    }

    WaylandCursorSource::WaylandCursorSource() : wakeEvent(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

    WaylandCursorSource::~WaylandCursorSource() = default;

    Result<> WaylandCursorSource::start(const CaptureTarget& target) {
        if (connection) {
            return failure(ErrorCode::INVALID_STATE, "The cursor source is already started");
        }
        if (target.outputName.empty()) {
            return failure(ErrorCode::INVALID_ARGUMENT, "The cursor source needs an output name");
        }
        if (!wakeEvent.isOpen()) {
            return failure(ErrorCode::DEVICE_FAILURE, "Could not create the cursor source's wake-up event");
        }

        auto created = std::make_unique<WaylandCursorConnection>();
        created->display = ::wl_display_connect(nullptr);
        if (created->display == nullptr) {
            return failure(ErrorCode::NOT_SUPPORTED, "There is no Wayland display to follow the cursor on");
        }
        created->registry = ::wl_display_get_registry(created->display);
        ::wl_registry_add_listener(created->registry, &REGISTRY_LISTENER, created.get());
        if (::wl_display_roundtrip(created->display) < 0 || ::wl_display_roundtrip(created->display) < 0) {
            return failure(ErrorCode::DEVICE_FAILURE, "The Wayland connection failed while listing its globals");
        }
        if (created->captureManager == nullptr || created->sourceManager == nullptr) {
            return failure(ErrorCode::NOT_SUPPORTED,
                           "The compositor does not offer cursor capture (ext-image-copy-capture-v1)");
        }
        if (created->seat == nullptr || (created->seatCapabilities & WL_SEAT_CAPABILITY_POINTER) == 0) {
            return failure(ErrorCode::NOT_SUPPORTED, "The compositor has no pointer to follow");
        }
        const auto found =
                std::ranges::find(created->outputs, target.outputName, [](const auto& output) { return output->name; });
        if (found == created->outputs.end()) {
            return failure(ErrorCode::INVALID_ARGUMENT,
                           std::format("The compositor has no output named {}", target.outputName));
        }
        created->target = found->get();
        created->theme = cursorThemeFromEnvironment();
        created->searchPath = cursorSearchPath();
        created->reloadImageForScale();

        created->pointer = ::wl_seat_get_pointer(created->seat);
        created->source = ::ext_output_image_capture_source_manager_v1_create_source(created->sourceManager,
                                                                                     created->target->output);
        created->cursorSession = ::ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
                created->captureManager, created->source, created->pointer);
        ::ext_image_copy_capture_cursor_session_v1_add_listener(created->cursorSession, &CURSOR_LISTENER,
                                                                created.get());
        if (::wl_display_roundtrip(created->display) < 0) {
            return failure(ErrorCode::DEVICE_FAILURE, "The compositor refused the cursor session");
        }
        connection = std::move(created);
        return {};
    }

    void WaylandCursorSource::stop() {
        connection.reset();
    }

    CursorStatus WaylandCursorSource::waitForChange(CursorState& state, std::chrono::milliseconds timeout) noexcept {
        if (!connection || connection->lost) {
            return CursorStatus::CURSOR_LOST;
        }
        WaylandCursorConnection& current = *connection;
        if (::wl_display_dispatch_pending(current.display) < 0) {
            return CursorStatus::CURSOR_LOST;
        }
        if (current.tracker.takeChange(state)) {
            return CursorStatus::CHANGED;
        }

        while (::wl_display_prepare_read(current.display) != 0) {
            if (::wl_display_dispatch_pending(current.display) < 0) {
                return CursorStatus::CURSOR_LOST;
            }
        }
        if (::wl_display_flush(current.display) < 0 && errno != EAGAIN) {
            ::wl_display_cancel_read(current.display);
            return CursorStatus::CURSOR_LOST;
        }
        std::array<pollfd, 2> descriptors{{{.fd = ::wl_display_get_fd(current.display), .events = POLLIN, .revents = 0},
                                           {.fd = wakeEvent.get(), .events = POLLIN, .revents = 0}}};
        const int ready = ::poll(descriptors.data(), descriptors.size(), static_cast<int>(timeout.count()));
        if (ready > 0 && descriptors[0].revents != 0) {
            if (::wl_display_read_events(current.display) < 0) {
                return CursorStatus::CURSOR_LOST;
            }
        } else {
            ::wl_display_cancel_read(current.display);
        }
        if (ready > 0 && (descriptors[1].revents & POLLIN) != 0) {
            std::uint64_t count = 0;
            static_cast<void>(::read(wakeEvent.get(), &count, sizeof(count)));
        }
        if (::wl_display_dispatch_pending(current.display) < 0 || current.lost) {
            return CursorStatus::CURSOR_LOST;
        }
        return current.tracker.takeChange(state) ? CursorStatus::CHANGED : CursorStatus::UNCHANGED;
    }

    void WaylandCursorSource::wake() noexcept {
        const std::uint64_t one = 1;
        static_cast<void>(::write(wakeEvent.get(), &one, sizeof(one)));
    }

    const CursorImage& WaylandCursorSource::image() const noexcept {
        return connection ? connection->image : EMPTY_IMAGE;
    }
}
