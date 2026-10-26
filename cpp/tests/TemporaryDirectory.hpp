#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

class TemporaryDirectory {
private:
    std::filesystem::path directoryPath;

public:
    TemporaryDirectory()
        : directoryPath(std::filesystem::temp_directory_path() /
                        ("fm-test-" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(directoryPath);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(directoryPath, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const {
        return directoryPath;
    }
};
