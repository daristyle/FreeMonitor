#include "linux/PortalScreenCast.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fm {
    namespace {
        constexpr const char* PORTAL_SERVICE = "org.freedesktop.portal.Desktop";
        constexpr const char* PORTAL_PATH = "/org/freedesktop/portal/desktop";
        constexpr const char* SCREEN_CAST_INTERFACE = "org.freedesktop.portal.ScreenCast";
        constexpr const char* REQUEST_INTERFACE = "org.freedesktop.portal.Request";
        constexpr const char* SESSION_INTERFACE = "org.freedesktop.portal.Session";

        constexpr std::uint32_t SOURCE_TYPE_MONITOR = 1;
        constexpr std::uint32_t CURSOR_MODE_HIDDEN = 1;
        constexpr std::uint32_t CURSOR_MODE_EMBEDDED = 2;
        constexpr std::uint32_t PERSIST_MODE_UNTIL_REVOKED = 2;
        constexpr std::uint32_t FIRST_VERSION_WITH_RESTORE_TOKEN = 4;
        constexpr std::uint32_t RESPONSE_SUCCESS = 0;
        constexpr std::uint32_t RESPONSE_CANCELLED = 1;

        constexpr std::uint64_t METHOD_CALL_TIMEOUT_US = 10'000'000;
        constexpr std::chrono::seconds REQUEST_TIMEOUT{30};
        constexpr std::chrono::minutes PICKER_TIMEOUT{5};

        struct MessageDeleter {
            void operator()(sd_bus_message* message) const {
                ::sd_bus_message_unref(message);
            }
        };
        using MessagePtr = std::unique_ptr<sd_bus_message, MessageDeleter>;

        struct SlotDeleter {
            void operator()(sd_bus_slot* slot) const {
                ::sd_bus_slot_unref(slot);
            }
        };
        using SlotPtr = std::unique_ptr<sd_bus_slot, SlotDeleter>;

        class BusError {
        private:
            sd_bus_error error{};

        public:
            BusError() = default;

            ~BusError() {
                ::sd_bus_error_free(&error);
            }

            BusError(const BusError&) = delete;
            BusError& operator=(const BusError&) = delete;

            sd_bus_error* get() {
                return &error;
            }

            bool hasName(const char* name) const {
                return ::sd_bus_error_has_name(&error, name) != 0;
            }

            std::string describe(int result) const {
                if (error.message != nullptr) {
                    return error.message;
                }
                if (error.name != nullptr) {
                    return error.name;
                }
                return std::system_category().message(-result);
            }
        };

        struct PendingResponse {
            bool received = false;
            std::uint32_t code = 0;
            MessagePtr message;
        };

        using EntryHandler = std::function<int(std::string_view key, std::string_view contents, sd_bus_message*)>;

        Error busError(const char* operation, int result) {
            return Error{ErrorCode::DEVICE_FAILURE,
                         std::string(operation) + ": " + std::system_category().message(-result)};
        }

        Error callError(const char* method, int result, const BusError& error) {
            if (error.hasName(SD_BUS_ERROR_SERVICE_UNKNOWN) || error.hasName(SD_BUS_ERROR_UNKNOWN_OBJECT) ||
                error.hasName(SD_BUS_ERROR_UNKNOWN_INTERFACE) || error.hasName(SD_BUS_ERROR_UNKNOWN_PROPERTY)) {
                return Error{ErrorCode::NOT_SUPPORTED,
                             "xdg-desktop-portal with ScreenCast support is not available: " + error.describe(result)};
            }
            return Error{ErrorCode::DEVICE_FAILURE,
                         std::string("Portal ") + method + " failed: " + error.describe(result)};
        }

        int onResponse(sd_bus_message* message, void* userData, sd_bus_error*) {
            auto* pending = static_cast<PendingResponse*>(userData);
            std::uint32_t code = 2;
            if (::sd_bus_message_read(message, "u", &code) < 0) {
                code = 2;
            }
            pending->code = code;
            pending->message.reset(::sd_bus_message_ref(message));
            pending->received = true;
            return 0;
        }

        Result<> waitForResponse(sd_bus* bus, const PendingResponse& pending,
                                 std::chrono::steady_clock::duration timeout, const char* method) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!pending.received) {
                const int processed = ::sd_bus_process(bus, nullptr);
                if (processed < 0) {
                    return std::unexpected(busError("sd_bus_process", processed));
                }
                if (processed > 0) {
                    continue;
                }
                const auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining <= std::chrono::steady_clock::duration::zero()) {
                    return std::unexpected(
                            Error{ErrorCode::DEVICE_FAILURE, std::string("Portal ") + method + " timed out"});
                }
                const auto remainingMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(remaining);
                const int waited = ::sd_bus_wait(bus, static_cast<std::uint64_t>(remainingMicroseconds.count()));
                if (waited < 0) {
                    return std::unexpected(busError("sd_bus_wait", waited));
                }
            }
            return {};
        }

        int readDictionary(sd_bus_message* message, const EntryHandler& handler) {
            int result = ::sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
            if (result < 0) {
                return result;
            }
            while (true) {
                result = ::sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
                if (result <= 0) {
                    break;
                }
                const char* key = nullptr;
                result = ::sd_bus_message_read(message, "s", &key);
                if (result < 0) {
                    return result;
                }
                const char* contents = nullptr;
                result = ::sd_bus_message_peek_type(message, nullptr, &contents);
                if (result < 0) {
                    return result;
                }
                result = ::sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, contents);
                if (result < 0) {
                    return result;
                }
                result = handler(key, contents, message);
                if (result == 0) {
                    result = ::sd_bus_message_skip(message, contents);
                }
                if (result < 0) {
                    return result;
                }
                result = ::sd_bus_message_exit_container(message);
                if (result < 0) {
                    return result;
                }
                result = ::sd_bus_message_exit_container(message);
                if (result < 0) {
                    return result;
                }
            }
            if (result < 0) {
                return result;
            }
            return ::sd_bus_message_exit_container(message);
        }

        int readString(sd_bus_message* message, std::string_view contents, std::string& target) {
            if (contents != "s" && contents != "o") {
                return 0;
            }
            const char* value = nullptr;
            const int result = ::sd_bus_message_read_basic(message, contents[0], &value);
            if (result > 0 && value != nullptr) {
                target = value;
            }
            return result;
        }

        int readStreams(sd_bus_message* message, ScreenCastStream& stream, bool& found) {
            int result = ::sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(ua{sv})");
            if (result < 0) {
                return result;
            }
            while (true) {
                result = ::sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "ua{sv}");
                if (result <= 0) {
                    break;
                }
                std::uint32_t nodeId = 0;
                result = ::sd_bus_message_read(message, "u", &nodeId);
                if (result < 0) {
                    return result;
                }
                std::int32_t width = 0;
                std::int32_t height = 0;
                result = readDictionary(message,
                                        [&](std::string_view key, std::string_view contents, sd_bus_message* entry) {
                                            if (key == "size" && contents == "(ii)") {
                                                return ::sd_bus_message_read(entry, "(ii)", &width, &height);
                                            }
                                            return 0;
                                        });
                if (result < 0) {
                    return result;
                }
                if (!found) {
                    stream.nodeId = nodeId;
                    stream.width = static_cast<std::uint32_t>(std::max(width, 0));
                    stream.height = static_cast<std::uint32_t>(std::max(height, 0));
                    found = true;
                }
                result = ::sd_bus_message_exit_container(message);
                if (result < 0) {
                    return result;
                }
            }
            if (result < 0) {
                return result;
            }
            return ::sd_bus_message_exit_container(message);
        }

        Result<MessagePtr> newMethodCall(sd_bus* bus, const char* path, const char* interface, const char* member) {
            sd_bus_message* call = nullptr;
            const int result = ::sd_bus_message_new_method_call(bus, &call, PORTAL_SERVICE, path, interface, member);
            if (result < 0) {
                return std::unexpected(busError("sd_bus_message_new_method_call", result));
            }
            return MessagePtr(call);
        }

        Result<MessagePtr> callRequest(sd_bus* bus, std::string_view senderName, const std::string& handleToken,
                                       sd_bus_message* call, std::chrono::steady_clock::duration timeout,
                                       const char* method) {
            PendingResponse pending;
            const std::string expectedPath = PortalScreenCast::requestPath(senderName, handleToken);

            sd_bus_slot* rawSlot = nullptr;
            int result = ::sd_bus_match_signal(bus, &rawSlot, nullptr, expectedPath.c_str(), REQUEST_INTERFACE,
                                               "Response", onResponse, &pending);
            const SlotPtr slot(rawSlot);
            if (result < 0) {
                return std::unexpected(busError("sd_bus_match_signal", result));
            }

            BusError error;
            sd_bus_message* rawReply = nullptr;
            result = ::sd_bus_call(bus, call, METHOD_CALL_TIMEOUT_US, error.get(), &rawReply);
            const MessagePtr reply(rawReply);
            if (result < 0) {
                return std::unexpected(callError(method, result, error));
            }

            const char* returnedPath = nullptr;
            SlotPtr fallbackSlot;
            if (::sd_bus_message_read(reply.get(), "o", &returnedPath) > 0 && expectedPath != returnedPath) {
                sd_bus_slot* rawFallbackSlot = nullptr;
                result = ::sd_bus_match_signal(bus, &rawFallbackSlot, nullptr, returnedPath, REQUEST_INTERFACE,
                                               "Response", onResponse, &pending);
                fallbackSlot.reset(rawFallbackSlot);
                if (result < 0) {
                    return std::unexpected(busError("sd_bus_match_signal", result));
                }
            }

            if (auto waited = waitForResponse(bus, pending, timeout, method); !waited) {
                return std::unexpected(waited.error());
            }
            if (pending.code == RESPONSE_CANCELLED) {
                return std::unexpected(Error{ErrorCode::PERMISSION_DENIED, "Screen sharing was cancelled"});
            }
            if (pending.code != RESPONSE_SUCCESS) {
                return std::unexpected(
                        Error{ErrorCode::DEVICE_FAILURE, std::string("Portal ") + method + " request failed"});
            }

            std::uint32_t code = 0;
            result = ::sd_bus_message_rewind(pending.message.get(), 1);
            if (result >= 0) {
                result = ::sd_bus_message_read(pending.message.get(), "u", &code);
            }
            if (result < 0) {
                return std::unexpected(busError("Reading the portal response", result));
            }
            return std::move(pending.message);
        }

        Result<std::uint32_t> readUnsignedProperty(sd_bus* bus, const char* property) {
            BusError error;
            std::uint32_t value = 0;
            const int result = ::sd_bus_get_property_trivial(bus, PORTAL_SERVICE, PORTAL_PATH, SCREEN_CAST_INTERFACE,
                                                             property, error.get(), SD_BUS_TYPE_UINT32, &value);
            if (result < 0) {
                return std::unexpected(callError(property, result, error));
            }
            return value;
        }
    }

    PortalScreenCast::~PortalScreenCast() {
        close();
    }

    std::string PortalScreenCast::senderNameFromUniqueName(std::string_view uniqueName) {
        if (!uniqueName.empty() && uniqueName.front() == ':') {
            uniqueName.remove_prefix(1);
        }
        std::string senderName(uniqueName);
        std::replace(senderName.begin(), senderName.end(), '.', '_');
        return senderName;
    }

    std::string PortalScreenCast::requestPath(std::string_view senderName, std::string_view handleToken) {
        std::string path = "/org/freedesktop/portal/desktop/request/";
        path += senderName;
        path += '/';
        path += handleToken;
        return path;
    }

    std::string PortalScreenCast::nextToken(std::string_view prefix) {
        std::string token(prefix);
        token += '_';
        token += std::to_string(++tokenCounter);
        return token;
    }

    Result<ScreenCastStream> PortalScreenCast::start(const std::optional<std::string>& restoreToken) {
        if (bus != nullptr) {
            return std::unexpected(Error{ErrorCode::INVALID_STATE, "Portal session is already started"});
        }
        const int result = ::sd_bus_open_user(&bus);
        if (result < 0) {
            bus = nullptr;
            return std::unexpected(busError("Cannot connect to the session D-Bus", result));
        }

        auto fail = [this](Error error) {
            close();
            return std::unexpected(std::move(error));
        };

        const char* uniqueName = nullptr;
        if (const int nameResult = ::sd_bus_get_unique_name(bus, &uniqueName); nameResult < 0) {
            return fail(busError("sd_bus_get_unique_name", nameResult));
        }
        senderName = senderNameFromUniqueName(uniqueName);

        const auto version = readUnsignedProperty(bus, "version");
        if (!version) {
            return fail(version.error());
        }
        const auto cursorModes = readUnsignedProperty(bus, "AvailableCursorModes");
        if (!cursorModes) {
            return fail(cursorModes.error());
        }
        const std::uint32_t cursorMode =
                (*cursorModes & CURSOR_MODE_EMBEDDED) != 0 ? CURSOR_MODE_EMBEDDED : CURSOR_MODE_HIDDEN;
        const bool supportsRestoreToken = *version >= FIRST_VERSION_WITH_RESTORE_TOKEN;

        if (auto created = createSession(); !created) {
            return fail(created.error());
        }
        if (auto selected = selectSources(cursorMode, supportsRestoreToken, restoreToken); !selected) {
            return fail(selected.error());
        }
        auto stream = startStream();
        if (!stream) {
            return fail(stream.error());
        }
        const auto descriptor = openPipeWireRemote();
        if (!descriptor) {
            return fail(descriptor.error());
        }
        stream->pipeWireDescriptor = *descriptor;
        return stream;
    }

    Result<> PortalScreenCast::createSession() {
        auto call = newMethodCall(bus, PORTAL_PATH, SCREEN_CAST_INTERFACE, "CreateSession");
        if (!call) {
            return std::unexpected(call.error());
        }
        const std::string handleToken = nextToken("fm_request");
        const std::string sessionToken = nextToken("fm_session");
        int result = ::sd_bus_message_append(call->get(), "a{sv}", 2, "handle_token", "s", handleToken.c_str(),
                                             "session_handle_token", "s", sessionToken.c_str());
        if (result < 0) {
            return std::unexpected(busError("Building CreateSession", result));
        }
        auto response = callRequest(bus, senderName, handleToken, call->get(), REQUEST_TIMEOUT, "CreateSession");
        if (!response) {
            return std::unexpected(response.error());
        }
        result = readDictionary(response->get(),
                                [this](std::string_view key, std::string_view contents, sd_bus_message* entry) {
                                    return key == "session_handle" ? readString(entry, contents, sessionHandle) : 0;
                                });
        if (result < 0 || sessionHandle.empty()) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Portal returned no session handle"});
        }
        return {};
    }

    Result<> PortalScreenCast::selectSources(std::uint32_t cursorMode, bool supportsRestoreToken,
                                             const std::optional<std::string>& restoreToken) {
        auto call = newMethodCall(bus, PORTAL_PATH, SCREEN_CAST_INTERFACE, "SelectSources");
        if (!call) {
            return std::unexpected(call.error());
        }
        sd_bus_message* message = call->get();
        const std::string handleToken = nextToken("fm_request");
        int result = ::sd_bus_message_append(message, "o", sessionHandle.c_str());
        if (result >= 0) {
            result = ::sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
        }
        if (result >= 0) {
            result = ::sd_bus_message_append(message, "{sv}{sv}{sv}{sv}", "handle_token", "s", handleToken.c_str(),
                                             "types", "u", SOURCE_TYPE_MONITOR, "multiple", "b", 0, "cursor_mode", "u",
                                             cursorMode);
        }
        if (result >= 0 && supportsRestoreToken) {
            result = ::sd_bus_message_append(message, "{sv}", "persist_mode", "u", PERSIST_MODE_UNTIL_REVOKED);
        }
        if (result >= 0 && supportsRestoreToken && restoreToken) {
            result = ::sd_bus_message_append(message, "{sv}", "restore_token", "s", restoreToken->c_str());
        }
        if (result >= 0) {
            result = ::sd_bus_message_close_container(message);
        }
        if (result < 0) {
            return std::unexpected(busError("Building SelectSources", result));
        }
        auto response = callRequest(bus, senderName, handleToken, message, REQUEST_TIMEOUT, "SelectSources");
        if (!response) {
            return std::unexpected(response.error());
        }
        return {};
    }

    Result<ScreenCastStream> PortalScreenCast::startStream() {
        auto call = newMethodCall(bus, PORTAL_PATH, SCREEN_CAST_INTERFACE, "Start");
        if (!call) {
            return std::unexpected(call.error());
        }
        const std::string handleToken = nextToken("fm_request");
        int result = ::sd_bus_message_append(call->get(), "osa{sv}", sessionHandle.c_str(), "", 1, "handle_token", "s",
                                             handleToken.c_str());
        if (result < 0) {
            return std::unexpected(busError("Building Start", result));
        }
        auto response = callRequest(bus, senderName, handleToken, call->get(), PICKER_TIMEOUT, "Start");
        if (!response) {
            return std::unexpected(response.error());
        }

        ScreenCastStream stream;
        bool found = false;
        std::string newRestoreToken;
        result = readDictionary(response->get(),
                                [&](std::string_view key, std::string_view contents, sd_bus_message* entry) {
                                    if (key == "streams" && contents == "a(ua{sv})") {
                                        const int streamsResult = readStreams(entry, stream, found);
                                        return streamsResult < 0 ? streamsResult : 1;
                                    }
                                    if (key == "restore_token") {
                                        return readString(entry, contents, newRestoreToken);
                                    }
                                    return 0;
                                });
        if (result < 0) {
            return std::unexpected(busError("Reading the Start response", result));
        }
        if (!found) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Portal returned no screen stream"});
        }
        if (!newRestoreToken.empty()) {
            stream.restoreToken = std::move(newRestoreToken);
        }
        return stream;
    }

    Result<int> PortalScreenCast::openPipeWireRemote() {
        auto call = newMethodCall(bus, PORTAL_PATH, SCREEN_CAST_INTERFACE, "OpenPipeWireRemote");
        if (!call) {
            return std::unexpected(call.error());
        }
        int result = ::sd_bus_message_append(call->get(), "oa{sv}", sessionHandle.c_str(), 0);
        if (result < 0) {
            return std::unexpected(busError("Building OpenPipeWireRemote", result));
        }
        BusError error;
        sd_bus_message* rawReply = nullptr;
        result = ::sd_bus_call(bus, call->get(), METHOD_CALL_TIMEOUT_US, error.get(), &rawReply);
        const MessagePtr reply(rawReply);
        if (result < 0) {
            return std::unexpected(callError("OpenPipeWireRemote", result, error));
        }
        int borrowedDescriptor = -1;
        result = ::sd_bus_message_read(reply.get(), "h", &borrowedDescriptor);
        if (result < 0) {
            return std::unexpected(busError("Reading the PipeWire descriptor", result));
        }
        const int ownedDescriptor = ::fcntl(borrowedDescriptor, F_DUPFD_CLOEXEC, 3);
        if (ownedDescriptor < 0) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot duplicate the PipeWire descriptor"});
        }
        return ownedDescriptor;
    }

    void PortalScreenCast::close() {
        if (bus == nullptr) {
            return;
        }
        if (!sessionHandle.empty()) {
            BusError ignoredError;
            ::sd_bus_call_method(bus, PORTAL_SERVICE, sessionHandle.c_str(), SESSION_INTERFACE, "Close",
                                 ignoredError.get(), nullptr, "");
        }
        bus = ::sd_bus_flush_close_unref(bus);
        sessionHandle.clear();
        senderName.clear();
    }
}
