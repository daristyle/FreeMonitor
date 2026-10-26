#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fm {
    struct VAAPIDevice {
        std::string path;
        std::string driver;
    };

    std::vector<VAAPIDevice> findVAAPIDevices(const std::filesystem::path& drmClassDirectory = "/sys/class/drm",
                                              const std::filesystem::path& deviceDirectory = "/dev/dri");
}
