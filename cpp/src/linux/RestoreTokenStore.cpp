#include "linux/RestoreTokenStore.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fm {
    namespace {
        class FileDescriptor {
        private:
            int descriptor;

        public:
            explicit FileDescriptor(int descriptor) : descriptor(descriptor) {}

            ~FileDescriptor() {
                if (descriptor >= 0) {
                    ::close(descriptor);
                }
            }

            FileDescriptor(const FileDescriptor&) = delete;
            FileDescriptor& operator=(const FileDescriptor&) = delete;

            bool isOpen() const {
                return descriptor >= 0;
            }

            int get() const {
                return descriptor;
            }

            int release() {
                return std::exchange(descriptor, -1);
            }
        };

        Error systemError(const char* operation, const std::filesystem::path& path) {
            const int code = errno;
            return Error{ErrorCode::DEVICE_FAILURE,
                         std::string(operation) + " " + path.string() + ": " + std::system_category().message(code)};
        }

        bool writeAll(int descriptor, std::string_view data) {
            while (!data.empty()) {
                const ssize_t written = ::write(descriptor, data.data(), data.size());
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    return false;
                }
                data.remove_prefix(static_cast<std::size_t>(written));
            }
            return true;
        }
    }

    RestoreTokenStore::RestoreTokenStore(std::filesystem::path filePath) : filePath(std::move(filePath)) {}

    Result<std::filesystem::path> RestoreTokenStore::defaultPath() {
        std::filesystem::path stateDirectory;
        if (const char* stateHome = std::getenv("XDG_STATE_HOME"); stateHome != nullptr && *stateHome == '/') {
            stateDirectory = stateHome;
        } else if (const char* home = std::getenv("HOME"); home != nullptr && *home == '/') {
            stateDirectory = std::filesystem::path(home) / ".local" / "state";
        } else {
            return std::unexpected(Error{ErrorCode::NOT_SUPPORTED, "Neither XDG_STATE_HOME nor HOME is set"});
        }
        return stateDirectory / "freemonitor" / "restore-token";
    }

    std::optional<std::string> RestoreTokenStore::load() const {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            return std::nullopt;
        }
        std::string token((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
            token.pop_back();
        }
        if (token.empty()) {
            return std::nullopt;
        }
        return token;
    }

    Result<> RestoreTokenStore::save(std::string_view token) const {
        const std::filesystem::path directoryPath = filePath.parent_path();
        std::error_code directoryError;
        std::filesystem::create_directories(directoryPath, directoryError);
        if (directoryError) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE,
                                         "Cannot create " + directoryPath.string() + ": " + directoryError.message()});
        }

        std::filesystem::path temporaryPath = filePath;
        temporaryPath += ".tmp";
        if (::unlink(temporaryPath.c_str()) != 0 && errno != ENOENT) {
            return std::unexpected(systemError("Cannot remove", temporaryPath));
        }

        auto failAndRemoveTemporary = [&temporaryPath](const char* operation) {
            const Error error = systemError(operation, temporaryPath);
            ::unlink(temporaryPath.c_str());
            return std::unexpected(error);
        };

        FileDescriptor file(::open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR));
        if (!file.isOpen()) {
            return std::unexpected(systemError("Cannot create", temporaryPath));
        }
        if (!writeAll(file.get(), token) || !writeAll(file.get(), "\n")) {
            return failAndRemoveTemporary("Cannot write");
        }
        if (::fsync(file.get()) != 0) {
            return failAndRemoveTemporary("Cannot sync");
        }
        if (::close(file.release()) != 0) {
            return failAndRemoveTemporary("Cannot close");
        }

        if (::rename(temporaryPath.c_str(), filePath.c_str()) != 0) {
            return failAndRemoveTemporary("Cannot rename");
        }

        const FileDescriptor directory(::open(directoryPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!directory.isOpen() || ::fsync(directory.get()) != 0) {
            return std::unexpected(systemError("Cannot sync", directoryPath));
        }
        return {};
    }
}
