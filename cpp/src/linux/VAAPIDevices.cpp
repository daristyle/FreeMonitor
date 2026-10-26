#include "linux/VAAPIDevices.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace fm {
    namespace {
        constexpr std::string_view RENDER_NODE_PREFIX = "renderD";
        constexpr std::array<std::pair<std::string_view, std::string_view>, 3> VAAPI_DRIVERS{{
                {"amdgpu", "radeonsi"},
                {"i915", "iHD"},
                {"xe", "iHD"},
        }};

        struct RenderNode {
            VAAPIDevice device;
            bool drivesBootDisplay = false;
        };

        std::optional<std::string_view> vaapiDriverFor(std::string_view kernelDriver) {
            for (const auto& [kernel, vaapi] : VAAPI_DRIVERS) {
                if (kernel == kernelDriver) {
                    return vaapi;
                }
            }
            return std::nullopt;
        }

        bool drivesBootDisplay(const std::filesystem::path& device) {
            std::ifstream file(device / "boot_vga");
            char value = '0';
            file >> value;
            return value == '1';
        }
    }

    std::vector<VAAPIDevice> findVAAPIDevices(const std::filesystem::path& drmClassDirectory,
                                              const std::filesystem::path& deviceDirectory) {
        std::vector<RenderNode> nodes;
        std::error_code listError;
        for (std::filesystem::directory_iterator entry(drmClassDirectory, listError);
             !listError && entry != std::filesystem::directory_iterator(); entry.increment(listError)) {
            const std::string name = entry->path().filename().string();
            if (!name.starts_with(RENDER_NODE_PREFIX)) {
                continue;
            }
            const std::filesystem::path device = entry->path() / "device";
            std::error_code linkError;
            const std::filesystem::path kernelDriver = std::filesystem::read_symlink(device / "driver", linkError);
            if (linkError) {
                continue;
            }
            if (const auto driver = vaapiDriverFor(kernelDriver.filename().string())) {
                nodes.push_back({{(deviceDirectory / name).string(), std::string(*driver)}, drivesBootDisplay(device)});
            }
        }

        std::ranges::sort(nodes, [](const RenderNode& left, const RenderNode& right) {
            if (left.drivesBootDisplay != right.drivesBootDisplay) {
                return left.drivesBootDisplay;
            }
            return left.device.path < right.device.path;
        });
        std::vector<VAAPIDevice> devices;
        devices.reserve(nodes.size());
        for (RenderNode& node : nodes) {
            devices.push_back(std::move(node.device));
        }
        return devices;
    }
}
