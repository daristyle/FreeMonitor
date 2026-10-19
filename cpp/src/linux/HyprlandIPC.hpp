#pragma once

#include "Error.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace fm {
    class HyprlandIPC {
    private:
        std::filesystem::path socketPath;
        std::chrono::milliseconds timeout;

    public:
        static constexpr std::chrono::milliseconds DEFAULT_TIMEOUT{2000};

        explicit HyprlandIPC(std::filesystem::path socketPath, std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);

        static Result<std::filesystem::path> defaultSocketPath();

        Result<std::string> request(std::string_view command) const;
    };
}
