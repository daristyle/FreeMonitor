#include "TemporaryDirectory.hpp"
#include "linux/VAAPIDevices.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
    struct FakeRenderNode {
        std::string_view name;
        std::string_view kernelDriver;
        bool drivesBootDisplay = false;
    };

    std::filesystem::path makeDRMClassDirectory(const TemporaryDirectory& root,
                                                const std::vector<FakeRenderNode>& nodes) {
        const std::filesystem::path drmClass = root.path() / "class" / "drm";
        const std::filesystem::path drivers = root.path() / "drivers";
        std::filesystem::create_directories(drmClass / "card1-eDP-1");
        for (const FakeRenderNode& node : nodes) {
            const std::filesystem::path device = drmClass / node.name / "device";
            std::filesystem::create_directories(device);
            std::filesystem::create_directories(drivers / node.kernelDriver);
            std::filesystem::create_directory_symlink(drivers / node.kernelDriver, device / "driver");
            std::ofstream(device / "boot_vga") << (node.drivesBootDisplay ? "1\n" : "0\n");
        }
        return drmClass;
    }

    std::vector<std::string> pathsOf(const std::vector<fm::VAAPIDevice>& devices) {
        std::vector<std::string> paths;
        for (const fm::VAAPIDevice& device : devices) {
            paths.push_back(device.path);
        }
        return paths;
    }
}

TEST_CASE("findVAAPIDevices finds the AMD GPU of a laptop with NVIDIA graphics and picks the Mesa driver") {
    const TemporaryDirectory root;
    const auto drmClass = makeDRMClassDirectory(root, {{"renderD128", "amdgpu", true}, {"renderD129", "nvidia"}});
    const auto devices = fm::findVAAPIDevices(drmClass, "/dev/dri");
    REQUIRE(devices.size() == 1);
    CHECK(devices.front().path == "/dev/dri/renderD128");
    CHECK(devices.front().driver == "radeonsi");
}

TEST_CASE("findVAAPIDevices picks Intel's media driver for both Intel kernel drivers") {
    const TemporaryDirectory root;
    const auto drmClass = makeDRMClassDirectory(root, {{"renderD128", "i915", true}, {"renderD129", "xe"}});
    const auto devices = fm::findVAAPIDevices(drmClass, "/dev/dri");
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].driver == "iHD");
    CHECK(devices[1].driver == "iHD");
}

TEST_CASE("findVAAPIDevices puts the GPU that drives the boot display first") {
    const TemporaryDirectory root;
    const auto drmClass = makeDRMClassDirectory(root, {{"renderD128", "i915"}, {"renderD129", "amdgpu", true}});
    CHECK(pathsOf(fm::findVAAPIDevices(drmClass, "/dev/dri")) ==
          std::vector<std::string>{"/dev/dri/renderD129", "/dev/dri/renderD128"});
}

TEST_CASE("findVAAPIDevices finds nothing without an AMD or Intel GPU") {
    const TemporaryDirectory root;
    SECTION("NVIDIA only") {
        CHECK(fm::findVAAPIDevices(makeDRMClassDirectory(root, {{"renderD128", "nvidia", true}}), "/dev/dri").empty());
    }
    SECTION("an unknown driver") {
        CHECK(fm::findVAAPIDevices(makeDRMClassDirectory(root, {{"renderD128", "virtio-pci"}}), "/dev/dri").empty());
    }
    SECTION("no GPU at all") {
        CHECK(fm::findVAAPIDevices(makeDRMClassDirectory(root, {}), "/dev/dri").empty());
    }
    SECTION("no DRM directory") {
        CHECK(fm::findVAAPIDevices(root.path() / "missing", "/dev/dri").empty());
    }
}
