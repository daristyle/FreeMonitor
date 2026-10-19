#pragma once

#include "DisplayLayout.hpp"
#include "VirtualDisplay.hpp"
#include "linux/HyprlandIPC.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fm {
    struct HyprlandMonitor {
        std::string name;
        std::string modeText;
        std::string scaleText = "1";
        int transform = 0;
        MonitorLayout layout;
    };

    class HyprlandVirtualDisplay final : public VirtualDisplay {
    private:
        static constexpr std::chrono::milliseconds POLL_INTERVAL{20};

        HyprlandIPC ipc;
        std::string outputName;
        std::chrono::milliseconds applyTimeout;
        bool created = false;

        Result<> command(const std::string& text) const;
        Result<std::vector<HyprlandMonitor>> readMonitors() const;
        Result<std::vector<HyprlandMonitor>> waitUntilApplied(const MonitorLayout& expected) const;
        void removeOutput() const;

    public:
        static constexpr std::string_view DEFAULT_OUTPUT_NAME = "FREEMONITOR-1";
        static constexpr std::chrono::milliseconds DEFAULT_APPLY_TIMEOUT{1000};

        HyprlandVirtualDisplay(HyprlandIPC ipc, std::string outputName,
                               std::chrono::milliseconds applyTimeout = DEFAULT_APPLY_TIMEOUT);
        ~HyprlandVirtualDisplay() override;

        Result<CaptureTarget> create(const DisplayMode& mode) override;
        void destroy() override;
    };

    std::optional<std::vector<HyprlandMonitor>> parseHyprlandMonitors(std::string_view reply);
}
