#include "CaptureSource.hpp"
#include "Encoder.hpp"
#include "VirtualDisplay.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <print>
#include <string_view>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::chrono::milliseconds CAPTURE_TIMEOUT{100};
    constexpr std::chrono::seconds REPORT_INTERVAL{1};
    constexpr int DEFAULT_RUN_SECONDS = 10;

    std::atomic<bool> stopRequested = false;

    void onStopSignal(int) {
        stopRequested.store(true);
    }

    double toMilliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    struct IntervalStatistics {
        Clock::duration minimum = Clock::duration::max();
        Clock::duration maximum = Clock::duration::zero();
        Clock::duration total = Clock::duration::zero();
        std::uint64_t count = 0;

        void add(Clock::duration value) {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            total += value;
            ++count;
        }

        double averageMilliseconds() const {
            return count == 0 ? 0.0 : toMilliseconds(total) / static_cast<double>(count);
        }

        double minimumMilliseconds() const {
            return count == 0 ? 0.0 : toMilliseconds(minimum);
        }

        double maximumMilliseconds() const {
            return toMilliseconds(maximum);
        }
    };

    std::optional<int> parseRunSeconds(int argumentCount, char** arguments) {
        int runSeconds = DEFAULT_RUN_SECONDS;
        for (int i = 1; i < argumentCount; ++i) {
            const std::string_view argument = arguments[i];
            if (argument != "--seconds" || i + 1 >= argumentCount) {
                return std::nullopt;
            }
            const std::string_view value = arguments[++i];
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), runSeconds);
            if (error != std::errc() || end != value.data() + value.size() || runSeconds < 0) {
                return std::nullopt;
            }
        }
        return runSeconds;
    }
}

int main(int argumentCount, char** arguments) {
    const auto runSeconds = parseRunSeconds(argumentCount, arguments);
    if (!runSeconds) {
        std::println(stderr, "Usage: FreeMonitorHost [--seconds N]   (N = 0 runs until Ctrl+C)");
        return 2;
    }

    const fm::DisplayMode displayMode{
            .width = 1920, .height = 1080, .refreshRateHz = 60, .positionX = 1920, .positionY = 0};
    auto virtualDisplay = fm::createVirtualDisplay();
    auto captureTarget = virtualDisplay->create(displayMode);
    if (!captureTarget) {
        std::println(stderr, "Could not create the virtual display: {}", captureTarget.error().message);
        return 1;
    }

    std::println("Starting capture. Choose a monitor if a screen sharing dialog appears.");
    auto captureSource = fm::createCaptureSource();
    if (auto started = captureSource->start(*captureTarget); !started) {
        std::println(stderr, "Could not start capture: {}", started.error().message);
        return 1;
    }
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    auto encoder = fm::createEncoder(fm::Codec::JPEG);
    fm::EncoderConfig encoderConfig;
    bool encoderConfigured = false;

    const auto startTime = Clock::now();
    const auto deadline = *runSeconds > 0 ? startTime + std::chrono::seconds(*runSeconds) : Clock::time_point::max();
    auto periodStart = startTime;
    std::optional<Clock::time_point> previousCaptureTime;
    IntervalStatistics frameIntervals;
    IntervalStatistics captureDelays;
    std::uint64_t periodFrames = 0;
    std::uint64_t encodeFailures = 0;
    int exitCode = 0;

    fm::CapturedFrame capturedFrame;
    fm::EncodedFrame encodedFrame;
    while (!stopRequested.load() && Clock::now() < deadline) {
        const auto status = captureSource->acquireFrame(capturedFrame, CAPTURE_TIMEOUT);
        if (status == fm::CaptureStatus::CAPTURE_LOST) {
            std::println(stderr, "Capture lost");
            exitCode = 1;
            break;
        }
        if (status == fm::CaptureStatus::FRAME_READY) {
            captureDelays.add(Clock::now() - capturedFrame.captureTime);
            if (previousCaptureTime) {
                frameIntervals.add(capturedFrame.captureTime - *previousCaptureTime);
            }
            previousCaptureTime = capturedFrame.captureTime;

            if (!encoderConfigured || capturedFrame.width != encoderConfig.width ||
                capturedFrame.height != encoderConfig.height || capturedFrame.format != encoderConfig.inputFormat) {
                encoderConfig.width = capturedFrame.width;
                encoderConfig.height = capturedFrame.height;
                encoderConfig.inputFormat = capturedFrame.format;
                encoderConfigured = encoder->configure(encoderConfig).has_value();
            }
            if (!encoderConfigured || encoder->encode(capturedFrame, encodedFrame) != fm::EncodeStatus::FRAME_ENCODED) {
                ++encodeFailures;
            }
            captureSource->releaseFrame();
            ++periodFrames;
        }

        const auto now = Clock::now();
        if (now - periodStart >= REPORT_INTERVAL) {
            const double periodSeconds = std::chrono::duration<double>(now - periodStart).count();
            std::println("{:4} fps | frame interval min {:6.2f} avg {:6.2f} max {:6.2f} ms | capture to acquire avg "
                         "{:5.2f} max {:5.2f} ms | {}x{} | encode failures {}",
                         static_cast<int>(static_cast<double>(periodFrames) / periodSeconds + 0.5),
                         frameIntervals.minimumMilliseconds(), frameIntervals.averageMilliseconds(),
                         frameIntervals.maximumMilliseconds(), captureDelays.averageMilliseconds(),
                         captureDelays.maximumMilliseconds(), encoderConfig.width, encoderConfig.height,
                         encodeFailures);
            periodStart = now;
            periodFrames = 0;
            frameIntervals = {};
            captureDelays = {};
        }
    }

    captureSource->stop();
    std::println("Stopped after {:.1f} s", std::chrono::duration<double>(Clock::now() - startTime).count());
    return exitCode;
}
