#include "linux/HyprlandIPC.hpp"

#include "linux/FileDescriptor.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <system_error>
#include <utility>

namespace fm {
    namespace {
        constexpr std::size_t RECEIVE_CHUNK_SIZE = 4096;

        Error socketError(std::string_view operation, const std::filesystem::path& socketPath) {
            const int code = errno;
            return Error{ErrorCode::DEVICE_FAILURE, std::string(operation) + " " + socketPath.string() + ": " +
                                                            std::system_category().message(code)};
        }

        bool setTimeout(int descriptor, int option, std::chrono::milliseconds timeout) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
            const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);
            const timeval value{.tv_sec = static_cast<time_t>(seconds.count()),
                                .tv_usec = static_cast<suseconds_t>(microseconds.count())};
            return ::setsockopt(descriptor, SOL_SOCKET, option, &value, sizeof(value)) == 0;
        }

        bool isValidSignature(std::string_view signature) {
            if (signature.empty()) {
                return false;
            }
            for (const char character : signature) {
                const bool allowed = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                                     (character >= '0' && character <= '9') || character == '_';
                if (!allowed) {
                    return false;
                }
            }
            return true;
        }
    }

    HyprlandIPC::HyprlandIPC(std::filesystem::path socketPath, std::chrono::milliseconds timeout)
        : socketPath(std::move(socketPath)), timeout(timeout) {}

    Result<std::filesystem::path> HyprlandIPC::defaultSocketPath() {
        const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
        const char* runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
        if (signature == nullptr || !isValidSignature(signature)) {
            return std::unexpected(Error{ErrorCode::NOT_SUPPORTED, "Not running under Hyprland"});
        }
        if (runtimeDirectory == nullptr || *runtimeDirectory != '/') {
            return std::unexpected(Error{ErrorCode::NOT_SUPPORTED, "XDG_RUNTIME_DIR is not set"});
        }
        return std::filesystem::path(runtimeDirectory) / "hypr" / signature / ".socket.sock";
    }

    Result<std::string> HyprlandIPC::request(std::string_view command) const {
        if (timeout <= std::chrono::milliseconds::zero()) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Hyprland request timeout must be positive"});
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string pathText = socketPath.string();
        if (pathText.size() >= sizeof(address.sun_path)) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT, "Hyprland socket path is too long: " + pathText});
        }
        std::memcpy(address.sun_path, pathText.c_str(), pathText.size() + 1);

        const FileDescriptor connection(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!connection.isOpen()) {
            return std::unexpected(socketError("Cannot create a socket for", socketPath));
        }
        if (!setTimeout(connection.get(), SO_RCVTIMEO, timeout) ||
            !setTimeout(connection.get(), SO_SNDTIMEO, timeout)) {
            return std::unexpected(socketError("Cannot set a timeout on", socketPath));
        }
        if (::connect(connection.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            return std::unexpected(socketError("Cannot connect to Hyprland at", socketPath));
        }

        std::string_view remaining = command;
        while (!remaining.empty()) {
            const ssize_t sent = ::send(connection.get(), remaining.data(), remaining.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(socketError("Cannot send to Hyprland at", socketPath));
            }
            remaining.remove_prefix(static_cast<std::size_t>(sent));
        }

        std::string reply;
        std::array<char, RECEIVE_CHUNK_SIZE> chunk{};
        while (true) {
            const ssize_t received = ::recv(connection.get(), chunk.data(), chunk.size(), 0);
            if (received == 0) {
                return reply;
            }
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return std::unexpected(Error{ErrorCode::DEVICE_FAILURE,
                                                 "Hyprland did not answer \"" + std::string(command) + "\""});
                }
                return std::unexpected(socketError("Cannot receive from Hyprland at", socketPath));
            }
            reply.append(chunk.data(), static_cast<std::size_t>(received));
        }
    }
}
