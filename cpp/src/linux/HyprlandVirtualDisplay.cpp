#include "linux/HyprlandVirtualDisplay.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <thread>
#include <utility>

namespace fm {
    namespace {
        constexpr std::string_view MONITOR_PREFIX = "Monitor ";
        constexpr std::string_view MONITOR_ID_SEPARATOR = " (ID ";
        constexpr std::string_view POSITION_SEPARATOR = " at ";
        constexpr std::string_view SCALE_PREFIX = "scale: ";
        constexpr std::string_view TRANSFORM_PREFIX = "transform: ";

        template<typename T>
        std::optional<T> parseValue(std::string_view text) {
            T value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
                return std::nullopt;
            }
            return value;
        }

        struct PendingMonitor {
            HyprlandMonitor monitor;
            std::optional<std::uint32_t> pixelWidth;
            std::uint32_t pixelHeight = 0;
            double scale = 1.0;
        };

        bool parseGeometry(std::string_view line, PendingMonitor& pending) {
            const std::size_t at = line.find(POSITION_SEPARATOR);
            const std::size_t sizeSeparator = line.find('x');
            const std::size_t refreshSeparator = line.find('@');
            if (at == std::string_view::npos || sizeSeparator == std::string_view::npos ||
                refreshSeparator == std::string_view::npos ||
                !(sizeSeparator < refreshSeparator && refreshSeparator < at)) {
                return false;
            }
            const std::string_view position = line.substr(at + POSITION_SEPARATOR.size());
            const std::size_t positionSeparator = position.find('x');
            if (positionSeparator == std::string_view::npos) {
                return false;
            }
            const auto width = parseValue<std::uint32_t>(line.substr(0, sizeSeparator));
            const auto height =
                    parseValue<std::uint32_t>(line.substr(sizeSeparator + 1, refreshSeparator - sizeSeparator - 1));
            const auto x = parseValue<std::int32_t>(position.substr(0, positionSeparator));
            const auto y = parseValue<std::int32_t>(position.substr(positionSeparator + 1));
            if (!width || !height || !x || !y) {
                return false;
            }
            pending.pixelWidth = *width;
            pending.pixelHeight = *height;
            pending.monitor.modeText = std::string(line.substr(0, at));
            pending.monitor.layout.x = *x;
            pending.monitor.layout.y = *y;
            return true;
        }

        std::optional<HyprlandMonitor> finish(PendingMonitor pending) {
            if (!pending.pixelWidth || !(pending.scale > 0.0)) {
                return std::nullopt;
            }
            auto width = static_cast<std::uint32_t>(std::lround(*pending.pixelWidth / pending.scale));
            auto height = static_cast<std::uint32_t>(std::lround(pending.pixelHeight / pending.scale));
            if (pending.monitor.transform % 2 == 1) {
                std::swap(width, height);
            }
            pending.monitor.layout.width = width;
            pending.monitor.layout.height = height;
            return std::move(pending.monitor);
        }

        std::vector<MonitorLayout> layoutsOf(const std::vector<HyprlandMonitor>& monitors) {
            std::vector<MonitorLayout> layouts;
            std::ranges::transform(monitors, std::back_inserter(layouts), &HyprlandMonitor::layout);
            return layouts;
        }

        std::string describeMovedMonitors(const std::vector<HyprlandMonitor>& before,
                                          const std::vector<HyprlandMonitor>& after, std::string_view outputName) {
            std::string moves;
            std::string fixedRules;
            for (const auto& old : before) {
                const auto current = std::ranges::find(after, old.name, &HyprlandMonitor::name);
                if (current == after.end() ||
                    (current->layout.x == old.layout.x && current->layout.y == old.layout.y)) {
                    continue;
                }
                moves += std::format("{} from {},{} to {},{}, ", old.name, old.layout.x, old.layout.y,
                                     current->layout.x, current->layout.y);
                fixedRules += std::format("  monitor = {}, {}, {}x{}, {}", old.name, old.modeText, old.layout.x,
                                          old.layout.y, old.scaleText);
                if (old.transform != 0) {
                    fixedRules += std::format(", transform, {}", old.transform);
                }
                fixedRules += '\n';
            }
            if (moves.empty()) {
                return {};
            }
            return std::format("Adding {} made Hyprland move {}because their monitor rules place them automatically. "
                               "Give them a fixed position in your Hyprland config, for example:\n{}"
                               "Or use --position left, which does not move automatically placed monitors.",
                               outputName, moves, fixedRules);
        }
    }

    std::optional<std::vector<HyprlandMonitor>> parseHyprlandMonitors(std::string_view reply) {
        std::vector<HyprlandMonitor> monitors;
        std::optional<PendingMonitor> pending;
        auto finishPending = [&]() {
            if (!pending) {
                return true;
            }
            auto monitor = finish(std::move(*pending));
            pending.reset();
            if (!monitor) {
                return false;
            }
            monitors.push_back(std::move(*monitor));
            return true;
        };

        while (!reply.empty()) {
            const std::size_t lineEnd = reply.find('\n');
            std::string_view line = reply.substr(0, lineEnd);
            reply.remove_prefix(lineEnd == std::string_view::npos ? reply.size() : lineEnd + 1);

            if (line.starts_with(MONITOR_PREFIX)) {
                if (!finishPending()) {
                    return std::nullopt;
                }
                const std::size_t idSeparator = line.rfind(MONITOR_ID_SEPARATOR);
                if (idSeparator == std::string_view::npos || idSeparator <= MONITOR_PREFIX.size()) {
                    return std::nullopt;
                }
                pending.emplace();
                pending->monitor.name =
                        std::string(line.substr(MONITOR_PREFIX.size(), idSeparator - MONITOR_PREFIX.size()));
                continue;
            }
            if (!pending || !line.starts_with('\t')) {
                continue;
            }
            line.remove_prefix(1);
            if (!pending->pixelWidth && line.find(POSITION_SEPARATOR) != std::string_view::npos) {
                if (!parseGeometry(line, *pending)) {
                    return std::nullopt;
                }
            } else if (line.starts_with(SCALE_PREFIX)) {
                const std::string_view scaleText = line.substr(SCALE_PREFIX.size());
                const auto scale = parseValue<double>(scaleText);
                if (!scale) {
                    return std::nullopt;
                }
                pending->scale = *scale;
                pending->monitor.scaleText = std::string(scaleText);
            } else if (line.starts_with(TRANSFORM_PREFIX)) {
                const auto transform = parseValue<int>(line.substr(TRANSFORM_PREFIX.size()));
                if (!transform) {
                    return std::nullopt;
                }
                pending->monitor.transform = *transform;
            }
        }
        if (!finishPending()) {
            return std::nullopt;
        }
        return monitors;
    }

    HyprlandVirtualDisplay::HyprlandVirtualDisplay(HyprlandIPC ipc, std::string outputName,
                                                   std::chrono::milliseconds applyTimeout)
        : ipc(std::move(ipc)), outputName(std::move(outputName)), applyTimeout(applyTimeout) {}

    HyprlandVirtualDisplay::~HyprlandVirtualDisplay() {
        destroy();
    }

    Result<CaptureTarget> HyprlandVirtualDisplay::create(const DisplayMode& mode) {
        if (created) {
            return std::unexpected(Error{ErrorCode::INVALID_STATE, "Virtual display already exists"});
        }
        if (mode.width == 0 || mode.height == 0 || mode.refreshRateHz == 0) {
            return std::unexpected(
                    Error{ErrorCode::INVALID_ARGUMENT, "Display size and refresh rate must not be zero"});
        }

        removeOutput();
        const auto before = readMonitors();
        if (!before) {
            return std::unexpected(before.error());
        }
        const DisplayPosition position = placeDisplay(layoutsOf(*before), mode);

        if (auto createdOutput = command("output create headless " + outputName); !createdOutput) {
            return std::unexpected(createdOutput.error());
        }
        const std::string monitorRule = std::format("keyword monitor {},{}x{}@{},{}x{},1", outputName, mode.width,
                                                    mode.height, mode.refreshRateHz, position.x, position.y);
        if (auto configured = command(monitorRule); !configured) {
            removeOutput();
            return std::unexpected(configured.error());
        }

        const auto after = waitUntilApplied(MonitorLayout{position.x, position.y, mode.width, mode.height});
        if (!after) {
            removeOutput();
            return std::unexpected(after.error());
        }
        if (const std::string moved = describeMovedMonitors(*before, *after, outputName); !moved.empty()) {
            removeOutput();
            return std::unexpected(Error{ErrorCode::INVALID_STATE, moved});
        }

        created = true;
        return CaptureTarget{outputName, mode.width, mode.height};
    }

    void HyprlandVirtualDisplay::destroy() {
        if (!created) {
            return;
        }
        removeOutput();
        created = false;
    }

    Result<> HyprlandVirtualDisplay::command(const std::string& text) const {
        const auto reply = ipc.request(text);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        if (*reply != "ok") {
            return std::unexpected(
                    Error{ErrorCode::DEVICE_FAILURE, "Hyprland refused \"" + text + "\": " + std::string(*reply)});
        }
        return {};
    }

    Result<std::vector<HyprlandMonitor>> HyprlandVirtualDisplay::readMonitors() const {
        const auto reply = ipc.request("monitors");
        if (!reply) {
            return std::unexpected(reply.error());
        }
        auto monitors = parseHyprlandMonitors(*reply);
        if (!monitors) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Cannot read the Hyprland monitor layout"});
        }
        return std::move(*monitors);
    }

    Result<std::vector<HyprlandMonitor>> HyprlandVirtualDisplay::waitUntilApplied(const MonitorLayout& expected) const {
        const auto deadline = std::chrono::steady_clock::now() + applyTimeout;
        while (true) {
            auto monitors = readMonitors();
            if (!monitors) {
                return std::unexpected(monitors.error());
            }
            const auto output = std::ranges::find(*monitors, outputName, &HyprlandMonitor::name);
            if (output != monitors->end() && output->layout == expected) {
                return std::move(*monitors);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected(Error{ErrorCode::DEVICE_FAILURE,
                                             std::format("Hyprland did not show {} as {}x{} at {},{} within {} ms",
                                                         outputName, expected.width, expected.height, expected.x,
                                                         expected.y, applyTimeout.count())});
            }
            std::this_thread::sleep_for(POLL_INTERVAL);
        }
    }

    void HyprlandVirtualDisplay::removeOutput() const {
        static_cast<void>(ipc.request("output remove " + outputName));
    }
}
