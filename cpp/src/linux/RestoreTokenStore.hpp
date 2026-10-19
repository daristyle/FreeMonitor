#pragma once

#include "Error.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fm {
    class RestoreTokenStore {
    private:
        std::filesystem::path filePath;

    public:
        explicit RestoreTokenStore(std::filesystem::path filePath);

        static Result<std::filesystem::path> defaultPath(std::string_view outputName);

        const std::filesystem::path& path() const {
            return filePath;
        }

        std::optional<std::string> load() const;
        Result<> save(std::string_view token) const;
    };
}
