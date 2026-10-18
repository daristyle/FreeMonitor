#include "CaptureSource.hpp"
#include "Encoder.hpp"
#include "HostOptions.hpp"
#include "Session.hpp"
#include "UDPSocket.hpp"
#include "VideoSender.hpp"
#include "VirtualDisplay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <print>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::chrono::milliseconds CAPTURE_TIMEOUT{100};
    constexpr std::chrono::seconds REPORT_INTERVAL{1};
    constexpr int SEND_BUFFER_SIZE = 4 * 1024 * 1024;
    constexpr double BITS_PER_MEGABIT = 1'000'000.0;
    constexpr double BYTES_PER_KILOBYTE = 1024.0;

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

    struct SizeStatistics {
        std::size_t maximum = 0;
        std::size_t total = 0;
        std::uint64_t count = 0;

        void add(std::size_t value) {
            maximum = std::max(maximum, value);
            total += value;
            ++count;
        }

        double averageKilobytes() const {
            return count == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(count) / BYTES_PER_KILOBYTE;
        }

        double maximumKilobytes() const {
            return static_cast<double>(maximum) / BYTES_PER_KILOBYTE;
        }
    };

    void printUsage() {
        if constexpr (fm::DEVELOPMENT_MODE_AVAILABLE) {
            std::println(stderr, "Usage: FreeMonitorHost [--seconds N] [--quality 1-100] [--destination IPv4:PORT]");
            std::println(stderr, "  --seconds 0 runs until Ctrl+C. --destination streams JPEG in development mode.");
        } else {
            std::println(stderr, "Usage: FreeMonitorHost [--seconds N] [--quality 1-100]");
            std::println(stderr, "  --seconds 0 runs until Ctrl+C.");
        }
    }

    void printEndpoint(const fm::Endpoint& endpoint) {
        std::println("{}.{}.{}.{}:{}", (endpoint.address >> 24) & 0xFF, (endpoint.address >> 16) & 0xFF,
                     (endpoint.address >> 8) & 0xFF, endpoint.address & 0xFF, endpoint.port);
    }
}

int main(int argumentCount, char** arguments) {
    const std::vector<std::string_view> argumentList(arguments + 1, arguments + argumentCount);
    const auto options = fm::parseHostOptions(argumentList, fm::DEVELOPMENT_MODE_AVAILABLE);
    if (!options) {
        std::println(stderr, "{}", options.error().message);
        printUsage();
        return 2;
    }

    std::optional<fm::UDPSocket> udpSocket;
    std::optional<fm::VideoSender> videoSender;
    fm::Session session;
    if (options->destination) {
        auto opened = fm::UDPSocket::open(fm::Endpoint{fm::ANY_IP_ADDRESS, 0});
        if (!opened) {
            std::println(stderr, "Could not open the UDP socket: {}", opened.error().message);
            return 1;
        }
        udpSocket.emplace(std::move(*opened));
        const auto sendBufferSize = udpSocket->setSendBufferSize(SEND_BUFFER_SIZE);
        if (!sendBufferSize || *sendBufferSize < SEND_BUFFER_SIZE) {
            std::println(stderr, "Warning: the send buffer is smaller than {} bytes. Raise net.core.wmem_max.",
                         SEND_BUFFER_SIZE);
        }

        std::random_device randomDevice;
        session = fm::Session{.peer = *options->destination, .sessionId = 0, .nextSeq = randomDevice()};
        videoSender.emplace(*udpSocket, randomDevice());
        std::print("Development mode: streaming JPEG with sessionId 0 to ");
        printEndpoint(*options->destination);
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
    encoderConfig.jpegQuality = options->jpegQuality;
    bool encoderConfigured = false;

    const auto startTime = Clock::now();
    const auto deadline =
            options->runSeconds > 0 ? startTime + std::chrono::seconds(options->runSeconds) : Clock::time_point::max();
    auto periodStart = startTime;
    std::optional<Clock::time_point> previousCaptureTime;
    IntervalStatistics frameIntervals;
    IntervalStatistics captureDelays;
    IntervalStatistics encodeTimes;
    IntervalStatistics captureToSendTimes;
    SizeStatistics encodedSizes;
    std::uint64_t periodFrames = 0;
    std::uint64_t encodeFailures = 0;
    std::uint64_t periodPackets = 0;
    std::uint64_t periodBytes = 0;
    std::uint64_t sendFailures = 0;
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
            const auto encodeStart = Clock::now();
            const bool encoded = encoderConfigured &&
                                 encoder->encode(capturedFrame, encodedFrame) == fm::EncodeStatus::FRAME_ENCODED;
            captureSource->releaseFrame();
            ++periodFrames;

            if (!encoded) {
                ++encodeFailures;
            } else {
                encodeTimes.add(Clock::now() - encodeStart);
                encodedSizes.add(encodedFrame.data.size());
                if (videoSender) {
                    const auto sendResult = videoSender->send(encodedFrame, fm::Codec::JPEG, session);
                    periodPackets += sendResult.packetsSent;
                    periodBytes += sendResult.bytesSent;
                    if (sendResult.status == fm::FrameSendStatus::FRAME_SENT) {
                        captureToSendTimes.add(Clock::now() - encodedFrame.captureTime);
                    } else {
                        ++sendFailures;
                    }
                }
            }
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
            std::println("     | encode avg {:6.2f} max {:6.2f} ms | JPEG avg {:7.1f} max {:7.1f} KB",
                         encodeTimes.averageMilliseconds(), encodeTimes.maximumMilliseconds(),
                         encodedSizes.averageKilobytes(), encodedSizes.maximumKilobytes());
            if (videoSender) {
                std::println("     | send {:7.1f} Mbit/s {:6} packets/s | capture to send avg {:6.2f} max {:6.2f} ms | "
                             "send failures {}",
                             static_cast<double>(periodBytes) * 8.0 / periodSeconds / BITS_PER_MEGABIT,
                             static_cast<int>(static_cast<double>(periodPackets) / periodSeconds + 0.5),
                             captureToSendTimes.averageMilliseconds(), captureToSendTimes.maximumMilliseconds(),
                             sendFailures);
            }
            periodStart = now;
            periodFrames = 0;
            periodPackets = 0;
            periodBytes = 0;
            frameIntervals = {};
            captureDelays = {};
            encodeTimes = {};
            captureToSendTimes = {};
            encodedSizes = {};
        }
    }

    captureSource->stop();
    std::println("Stopped after {:.1f} s", std::chrono::duration<double>(Clock::now() - startTime).count());
    return exitCode;
}
