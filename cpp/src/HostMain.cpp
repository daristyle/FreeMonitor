#include "CaptureSource.hpp"
#include "CursorSender.hpp"
#include "CursorSource.hpp"
#include "Encoder.hpp"
#include "FrameTimingLog.hpp"
#include "HostOptions.hpp"
#include "HostSessions.hpp"
#include "Session.hpp"
#include "Timestamp.hpp"
#include "UDPControlTransport.hpp"
#include "UDPSocket.hpp"
#include "VideoSender.hpp"
#include "VirtualDisplay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::chrono::milliseconds CAPTURE_TIMEOUT{10};
    constexpr std::chrono::seconds REPORT_INTERVAL{1};
    constexpr std::chrono::milliseconds KEYFRAME_INTERVAL{fm::DEVELOPMENT_KEYFRAME_INTERVAL_MS};
    constexpr std::size_t MAX_CONTROL_PACKETS_PER_PASS = 32;
    constexpr std::chrono::milliseconds BYE_WAIT_LIMIT{200};
    constexpr std::chrono::milliseconds BYE_WAIT_STEP{5};
    constexpr std::chrono::milliseconds IDLE_REFRESH_DELAY{150};
    constexpr std::chrono::milliseconds IDLE_REFRESH_MIN_INTERVAL{1000};
    constexpr int SEND_BUFFER_SIZE = 4 * 1024 * 1024;
    constexpr double BITS_PER_MEGABIT = 1'000'000.0;
    constexpr double BITS_PER_KILOBIT = 1'000.0;
    constexpr double BYTES_PER_KILOBYTE = 1024.0;
    constexpr double PERCENT = 100.0;

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
            std::println(stderr, "Usage: FreeMonitorHost [--seconds N] [--quality 1-100] [--size WxH] [--refresh HZ]");
            std::println(stderr,
                         "                       [--position right|left|above|below|X,Y] [--destination IPv4:PORT]");
            std::println(stderr, "                       [--encoder auto|nvenc|vaapi|jpeg] [--bitrate MBIT]");
            std::println(stderr,
                         "                       [--fec PERCENT] [--cursor auto|overlay|embedded] [--timing-log FILE]");
            std::println(stderr, "  --seconds 0 runs until Ctrl+C. --destination streams video in development mode.");
            std::println(stderr, "  Without --destination, the host waits for a tablet on UDP port {}.", fm::HOST_PORT);
        } else {
            std::println(stderr, "Usage: FreeMonitorHost [--seconds N] [--quality 1-100] [--size WxH] [--refresh HZ]");
            std::println(stderr, "                       [--position right|left|above|below|X,Y]");
            std::println(stderr, "                       [--encoder auto|nvenc|vaapi|jpeg] [--bitrate MBIT]");
            std::println(stderr,
                         "                       [--fec PERCENT] [--cursor auto|overlay|embedded] [--timing-log FILE]");
            std::println(stderr, "  --seconds 0 runs until Ctrl+C. The host waits for a tablet on UDP port {}.",
                         fm::HOST_PORT);
        }
        std::println(stderr, "  --encoder auto tries NVENC, then VA-API, then JPEG. It is the default.");
        std::println(stderr,
                     "  --bitrate sets the H.264 bitrate in Mbit/s (default 20). --quality sets the JPEG quality.");
        std::println(stderr, "  --fec sets the parity per frame in percent (default {}). 0 turns it off.",
                     fm::DEFAULT_FEC_PERCENT);
        std::println(stderr,
                     "  --cursor overlay lets the tablet draw the cursor, embedded keeps it in the video. auto (the");
        std::println(stderr,
                     "    default) uses the overlay when the desktop allows it, and embedded in development mode.");
        std::println(
                stderr,
                "    Hyprland draws the cursor into the virtual monitor's picture, so there it is always embedded.");
        std::println(stderr, "  --timing-log writes one CSV line of stage timestamps per frame.");
    }

    void printEndpoint(const fm::Endpoint& endpoint) {
        std::println("{}.{}.{}.{}:{}", (endpoint.address >> 24) & 0xFF, (endpoint.address >> 16) & 0xFF,
                     (endpoint.address >> 8) & 0xFF, endpoint.address & 0xFF, endpoint.port);
    }

    std::string_view codecName(fm::Codec codec) {
        switch (codec) {
            case fm::Codec::JPEG:
                return "JPEG";
            case fm::Codec::H264:
                return "H.264";
        }
        return "unknown codec";
    }

    std::string_view endReasonText(fm::SessionEndReason reason) {
        switch (reason) {
            case fm::SessionEndReason::REPLACED:
                return "another tablet took over";
            case fm::SessionEndReason::BYE:
                return "the tablet closed it";
            case fm::SessionEndReason::TIMEOUT:
                return "the tablet stopped answering";
            case fm::SessionEndReason::CLOSED:
                return "the host is closing";
        }
        return "unknown reason";
    }

    std::string_view rejectReasonText(fm::RejectReason reason) {
        switch (reason) {
            case fm::RejectReason::UNSUPPORTED_VERSION:
                return "it speaks another protocol version";
            case fm::RejectReason::NO_COMMON_CODEC:
                return "no codec in common";
            case fm::RejectReason::INVALID_PARAMETERS:
                return "its HELLO has invalid parameters";
            case fm::RejectReason::MISSING_FEATURE:
                return "it does not support the cursor overlay this host needs";
        }
        return "unknown reason";
    }

    class SessionEvents final : public fm::HostSessionListener {
    public:
        std::optional<fm::StreamInfo> startedStream;

        void sessionStarted(std::uint32_t sessionId, const fm::TabletInfo& tablet,
                            const fm::StreamInfo& stream) noexcept override {
            std::println("Tablet {:08X} connected: screen {}x{} at {} Hz. Streaming {} {}x{} at {} fps.", sessionId,
                         tablet.width, tablet.height, tablet.refreshHz, codecName(stream.codec), stream.width,
                         stream.height, stream.fps);
            startedStream = stream;
        }

        void streamingStarted(std::uint32_t sessionId) noexcept override {
            std::println("Tablet {:08X} is ready. Streaming started.", sessionId);
        }

        void sessionEnded(std::uint32_t sessionId, fm::SessionEndReason reason) noexcept override {
            std::println("Session {:08X} ended: {}.", sessionId, endReasonText(reason));
        }

        void helloRejected(std::uint32_t sessionId, fm::RejectReason reason) noexcept override {
            std::println("Rejected tablet {:08X}: {}.", sessionId, rejectReasonText(reason));
        }
    };

    struct EncoderSlot {
        std::unique_ptr<fm::Encoder> encoder;
        fm::EncoderConfig config;
        bool configured = false;

        bool needsConfigure(const fm::CapturedFrame& frame) const {
            return !configured || frame.width != config.width || frame.height != config.height ||
                   frame.format != config.inputFormat;
        }
    };

    fm::Result<> configureFor(EncoderSlot& slot, const fm::CapturedFrame& frame) {
        slot.config.width = frame.width;
        slot.config.height = frame.height;
        slot.config.inputFormat = frame.format;
        slot.configured = false;
        if (auto configured = slot.encoder->configure(slot.config); !configured) {
            return configured;
        }
        slot.configured = true;
        std::println("Encoding {}x{} with {}", slot.config.width, slot.config.height, slot.encoder->description());
        return {};
    }

    struct CursorTargetKey {
        std::uint32_t sessionId = 0;
        fm::Endpoint peer;

        friend bool operator==(const CursorTargetKey&, const CursorTargetKey&) = default;
    };

    fm::HostOffer offerFor(const EncoderSlot& slot, std::uint8_t fecPercent, bool cursorOverlay) {
        fm::HostOffer offer;
        offer.width = static_cast<std::uint16_t>(slot.config.width & ~1U);
        offer.height = static_cast<std::uint16_t>(slot.config.height & ~1U);
        offer.fps = static_cast<std::uint16_t>(slot.config.framesPerSecond);
        offer.codecs = slot.encoder->codec() == fm::Codec::H264
                               ? static_cast<std::uint8_t>(fm::CodecFlags::JPEG | fm::CodecFlags::H264)
                               : fm::CodecFlags::JPEG;
        offer.features = fecPercent > 0 ? fm::FeatureFlags::FEC : std::uint8_t{0};
        if (cursorOverlay) {
            offer.features |= fm::FeatureFlags::CURSOR_OVERLAY;
            offer.requiredFeatures = fm::FeatureFlags::CURSOR_OVERLAY;
        }
        return offer;
    }

    void receiveControlPackets(fm::UDPSocket& udpSocket, fm::HostSessions& sessions,
                               std::span<std::uint8_t> buffer) noexcept {
        fm::Endpoint from;
        for (std::size_t count = 0; count < MAX_CONTROL_PACKETS_PER_PASS; ++count) {
            const auto received = udpSocket.receiveFrom(buffer, from, std::chrono::milliseconds(0));
            if (received.status == fm::ReceiveStatus::NO_DATAGRAM ||
                received.status == fm::ReceiveStatus::RECEIVE_FAILED) {
                return;
            }
            if (received.status == fm::ReceiveStatus::RECEIVED) {
                sessions.handlePacket(buffer.first(received.size), from, received.arrivalTime);
            }
        }
    }

    void printTabletReport(const std::optional<fm::TabletReport>& report, std::optional<fm::Stats>& previousStats) {
        if (!report) {
            previousStats.reset();
            std::println("     | waiting for a tablet on UDP port {}", fm::HOST_PORT);
            return;
        }
        const std::string roundTrip =
                report->clockSample
                        ? std::format(
                                  "{:.1f} ms",
                                  std::chrono::duration<double, std::milli>(report->clockSample->roundTripTime).count())
                        : std::string("unknown");
        const bool sameSession = report->stats && previousStats &&
                                 previousStats->commonHeader.sessionId == report->stats->commonHeader.sessionId;
        const bool firstStats = report->stats && !sameSession;
        if (firstStats) {
            previousStats = report->stats;
            const fm::Stats& stats = *report->stats;
            std::println("     | tablet: RTT {} | first STATS: {} packets, {} lost, {} frames completed, {} dropped "
                         "since the session started",
                         roundTrip, stats.packetsReceived, stats.packetsLost, stats.framesCompleted,
                         stats.framesDropped);
            return;
        }
        std::optional<fm::StatsDifference> difference;
        if (report->stats) {
            difference = fm::statsDifference(*previousStats, *report->stats);
            previousStats = report->stats;
        }
        if (!difference) {
            std::println("     | tablet: RTT {} | no new STATS yet", roundTrip);
            return;
        }
        const double seconds = std::chrono::duration<double>(difference->interval).count();
        const auto lost = static_cast<double>(std::max<std::int32_t>(0, difference->packetsLost));
        const double expected = static_cast<double>(difference->packetsReceived) + lost;
        std::println("     | tablet: RTT {} | received {:6.1f} Mbit/s {:6} packets/s | loss {:5.2f}% | frames {} "
                     "completed {} dropped",
                     roundTrip, static_cast<double>(difference->bytesReceived) * 8.0 / seconds / BITS_PER_MEGABIT,
                     static_cast<int>(static_cast<double>(difference->packetsReceived) / seconds + 0.5),
                     expected > 0.0 ? lost / expected * PERCENT : 0.0, difference->framesCompleted,
                     difference->framesDropped);
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

    const fm::Endpoint bindTo{fm::ANY_IP_ADDRESS, options->destination ? std::uint16_t{0} : fm::HOST_PORT};
    auto opened = fm::UDPSocket::open(bindTo);
    if (!opened) {
        std::println(stderr, "Could not open the UDP socket: {}", opened.error().message);
        if (!options->destination) {
            std::println(stderr, "Is another FreeMonitor host already using UDP port {}?", fm::HOST_PORT);
        }
        return 1;
    }
    fm::UDPSocket udpSocket = std::move(*opened);
    const auto sendBufferSize = udpSocket.setSendBufferSize(SEND_BUFFER_SIZE);
    if (!sendBufferSize || *sendBufferSize < SEND_BUFFER_SIZE) {
        std::println(stderr, "Warning: the send buffer is smaller than {} bytes. Raise net.core.wmem_max.",
                     SEND_BUFFER_SIZE);
    }

    std::random_device randomDevice;
    fm::VideoSender videoSender(udpSocket, randomDevice());
    fm::Session developmentSession;
    fm::UDPControlTransport controlTransport(udpSocket);
    SessionEvents sessionEvents;
    std::optional<fm::HostSessions> sessions;
    if (options->destination) {
        developmentSession =
                fm::Session{.peer = *options->destination, .sessionId = 0, .seq = fm::SequenceCounter(randomDevice())};
        std::print("Development mode: streaming with sessionId 0 to ");
        printEndpoint(*options->destination);
    } else {
        sessions.emplace(controlTransport, sessionEvents, fm::HostOffer{},
                         [&randomDevice] { return static_cast<std::uint32_t>(randomDevice()); });
        std::println("Waiting for a tablet on UDP port {}.", fm::HOST_PORT);
    }

    std::unique_ptr<fm::FrameTimingLog> timingLog;
    if (options->timingLogPath) {
        auto openedLog = fm::FrameTimingLog::open(*options->timingLogPath);
        if (!openedLog) {
            std::println(stderr, "{}", openedLog.error().message);
            return 1;
        }
        timingLog = std::move(*openedLog);
        std::println("Writing frame timings to {} and clock offsets to {}", *options->timingLogPath,
                     fm::clockLogPath(*options->timingLogPath).string());
    }

    auto virtualDisplay = fm::createVirtualDisplay();
    auto captureTarget = virtualDisplay->create(options->displayMode);
    if (!captureTarget) {
        std::println(stderr, "Could not create the virtual display: {}", captureTarget.error().message);
        return 1;
    }
    std::println("Virtual monitor {} is {}x{} at {} Hz.", captureTarget->outputName, captureTarget->width,
                 captureTarget->height, options->displayMode.refreshRateHz);

    const bool developmentMode = options->destination.has_value();
    std::unique_ptr<fm::CursorSource> cursorSource;
    const bool overlayWanted = options->cursor == fm::CursorChoice::OVERLAY ||
                               (options->cursor == fm::CursorChoice::AUTO && !developmentMode);
    if (overlayWanted && captureTarget->cursorInPicture) {
        if (options->cursor == fm::CursorChoice::OVERLAY) {
            std::println(stderr,
                         "The desktop draws the cursor into the picture of {}, so the cursor overlay is not possible.",
                         captureTarget->outputName);
            return 1;
        }
        std::println("The cursor stays in the video: the desktop draws it into the picture of {}.",
                     captureTarget->outputName);
    } else if (overlayWanted) {
        cursorSource = fm::createCursorSource();
        if (auto started = cursorSource->start(*captureTarget); !started) {
            if (options->cursor == fm::CursorChoice::OVERLAY) {
                std::println(stderr, "Could not follow the cursor for the overlay: {}", started.error().message);
                return 1;
            }
            std::println("The cursor stays in the video: {}.", started.error().message);
            cursorSource.reset();
        }
    }
    captureTarget->hideCursor = cursorSource != nullptr;

    std::println("Starting capture. Share {} if a screen sharing dialog appears.", captureTarget->outputName);
    auto captureSource = fm::createCaptureSource();
    if (auto started = captureSource->start(*captureTarget); !started) {
        std::println(stderr, "Could not start capture: {}", started.error().message);
        return 1;
    }
    if (cursorSource && !captureSource->cursorHidden()) {
        if (options->cursor == fm::CursorChoice::OVERLAY) {
            std::println(stderr,
                         "The screen capture cannot leave the cursor out, so the cursor overlay is not possible.");
            return 1;
        }
        std::println("The screen capture keeps the cursor in the video, so the cursor overlay is off.");
        cursorSource.reset();
    }
    if (!cursorSource && captureSource->cursorHidden()) {
        std::println(stderr, "Warning: the screen capture leaves the cursor out, so the tablet shows no cursor.");
    }
    const bool cursorOverlay = cursorSource != nullptr;
    std::optional<fm::CursorSender> cursorSender;
    std::optional<CursorTargetKey> cursorTargetKey;
    if (cursorOverlay) {
        cursorSender.emplace(udpSocket, *cursorSource,
                             developmentMode ? std::optional{KEYFRAME_INTERVAL} : std::nullopt);
        if (developmentMode) {
            cursorSender->setTarget(
                    fm::CursorTarget{.peer = developmentSession.peer, .sessionId = 0, .seq = developmentSession.seq});
        }
        cursorSender->start();
        std::println("Cursor overlay on: the tablet draws the cursor{}.",
                     developmentMode ? "" : ", and a tablet that cannot is rejected");
    }
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    EncoderSlot mainEncoder;
    mainEncoder.encoder = fm::createEncoder(options->encoder);
    mainEncoder.config.jpegQuality = options->jpegQuality;
    mainEncoder.config.bitrateKbps = options->bitrateKbps;
    mainEncoder.config.framesPerSecond = options->displayMode.refreshRateHz;
    EncoderSlot jpegEncoder;
    jpegEncoder.config = mainEncoder.config;
    EncoderSlot* activeEncoder = &mainEncoder;
    std::optional<Clock::time_point> lastKeyframeTime;
    bool keyframeOwed = false;
    bool lastSentWasRepeat = true;
    std::optional<Clock::time_point> lastNewFrameSentTime;
    std::optional<Clock::time_point> lastRepeatTime;
    std::array<std::uint8_t, fm::MAX_CONTROL_PACKET_SIZE> controlBuffer{};
    std::optional<fm::Stats> reportedStats;
    std::optional<Clock::time_point> lastClockSampleTime;

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
    std::uint64_t periodRepeats = 0;
    std::uint64_t encodeFailures = 0;
    std::uint64_t periodPackets = 0;
    std::uint64_t periodBytes = 0;
    std::uint64_t sendFailures = 0;
    fm::CursorSendCounts reportedCursorCounts;
    bool cursorLossReported = false;
    int exitCode = 0;

    fm::CapturedFrame capturedFrame;
    fm::EncodedFrame encodedFrame;
    while (!stopRequested.load() && Clock::now() < deadline) {
        if (sessions && mainEncoder.configured) {
            receiveControlPackets(udpSocket, *sessions, controlBuffer);
            if (const auto stream = std::exchange(sessionEvents.startedStream, std::nullopt)) {
                if (stream->codec == mainEncoder.encoder->codec()) {
                    activeEncoder = &mainEncoder;
                } else {
                    if (!jpegEncoder.encoder) {
                        jpegEncoder.encoder = fm::createEncoder(fm::EncoderChoice::JPEG);
                    }
                    activeEncoder = &jpegEncoder;
                }
            }
            sessions->poll(Clock::now());
            if (sessions->takeKeyframeRequest()) {
                keyframeOwed = true;
            }
            if (cursorSender) {
                const fm::Session* streaming = sessions->streamingSession();
                std::optional<CursorTargetKey> key;
                if (streaming != nullptr && (streaming->features & fm::FeatureFlags::CURSOR_OVERLAY) != 0) {
                    key = CursorTargetKey{.sessionId = streaming->sessionId, .peer = streaming->peer};
                }
                if (key != cursorTargetKey) {
                    cursorTargetKey = key;
                    cursorSender->setTarget(key ? std::optional{fm::CursorTarget{.peer = streaming->peer,
                                                                                 .sessionId = streaming->sessionId,
                                                                                 .seq = streaming->seq}}
                                                : std::nullopt);
                }
                if (const auto shapeId = sessions->takeShapeRequest()) {
                    cursorSender->requestShape(*shapeId);
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
                         captureDelays.maximumMilliseconds(), mainEncoder.config.width, mainEncoder.config.height,
                         encodeFailures);
            std::println(
                    "     | encode avg {:6.2f} max {:6.2f} ms | frame avg {:7.1f} max {:7.1f} KB | repeated frames {}",
                    encodeTimes.averageMilliseconds(), encodeTimes.maximumMilliseconds(),
                    encodedSizes.averageKilobytes(), encodedSizes.maximumKilobytes(), periodRepeats);
            if (!sessions || sessions->streamingSession() != nullptr) {
                std::println("     | send {:7.1f} Mbit/s {:6} packets/s | capture to send avg {:6.2f} max {:6.2f} ms | "
                             "send failures {}",
                             static_cast<double>(periodBytes) * 8.0 / periodSeconds / BITS_PER_MEGABIT,
                             static_cast<int>(static_cast<double>(periodPackets) / periodSeconds + 0.5),
                             captureToSendTimes.averageMilliseconds(), captureToSendTimes.maximumMilliseconds(),
                             sendFailures);
            }
            if (cursorSender) {
                const fm::CursorSendCounts counts = cursorSender->counts();
                if (!sessions || sessions->streamingSession() != nullptr) {
                    std::println("     | cursor {:4} packets/s {:6.1f} kbit/s",
                                 static_cast<int>(static_cast<double>(counts.packets - reportedCursorCounts.packets) /
                                                          periodSeconds +
                                                  0.5),
                                 static_cast<double>(counts.bytes - reportedCursorCounts.bytes) * 8.0 / periodSeconds /
                                         BITS_PER_KILOBIT);
                }
                reportedCursorCounts = counts;
                if (cursorSender->lost() && !cursorLossReported) {
                    std::println(stderr, "The cursor source stopped, so the tablet gets no more cursor updates.");
                    cursorLossReported = true;
                }
            }
            if (sessions) {
                const auto report = sessions->report();
                printTabletReport(report, reportedStats);
                if (timingLog && report && report->clockSample &&
                    report->clockSample->receivedTime != lastClockSampleTime) {
                    lastClockSampleTime = report->clockSample->receivedTime;
                    timingLog->record(*report->clockSample);
                }
            }
            if (periodFrames == 0 && !virtualDisplay->stillExists()) {
                std::println(stderr, "The virtual monitor {} is gone, so the host is stopping.",
                             captureTarget->outputName);
                exitCode = 1;
                break;
            }
            periodStart = now;
            periodFrames = 0;
            periodRepeats = 0;
            periodPackets = 0;
            periodBytes = 0;
            frameIntervals = {};
            captureDelays = {};
            encodeTimes = {};
            captureToSendTimes = {};
            encodedSizes = {};
        }

        if (!sessions && lastKeyframeTime && now - *lastKeyframeTime >= KEYFRAME_INTERVAL) {
            keyframeOwed = true;
        }

        auto status = captureSource->acquireFrame(capturedFrame, CAPTURE_TIMEOUT);
        if (status == fm::CaptureStatus::CAPTURE_LOST) {
            std::println(stderr, "Capture lost");
            exitCode = 1;
            break;
        }
        bool repeated = false;
        if (status == fm::CaptureStatus::NO_NEW_FRAME && (!sessions || sessions->streamingSession() != nullptr)) {
            const auto idleTime = Clock::now();
            const bool refreshDue = !lastSentWasRepeat && lastNewFrameSentTime &&
                                    idleTime - *lastNewFrameSentTime >= IDLE_REFRESH_DELAY &&
                                    (!lastRepeatTime || idleTime - *lastRepeatTime >= IDLE_REFRESH_MIN_INTERVAL);
            if ((keyframeOwed || refreshDue) &&
                captureSource->repeatLastFrame(capturedFrame) == fm::CaptureStatus::FRAME_READY) {
                status = fm::CaptureStatus::FRAME_READY;
                repeated = true;
            }
        }
        if (status == fm::CaptureStatus::FRAME_READY) {
            const auto acquireTime = Clock::now();
            fm::FrameTiming timing{.captureUs = fm::toTimestampUs(capturedFrame.captureTime),
                                   .acquireUs = fm::toTimestampUs(acquireTime)};
            if (repeated) {
                ++periodRepeats;
            } else {
                captureDelays.add(acquireTime - capturedFrame.captureTime);
                if (previousCaptureTime) {
                    frameIntervals.add(capturedFrame.captureTime - *previousCaptureTime);
                }
                previousCaptureTime = capturedFrame.captureTime;
                ++periodFrames;
            }

            if (mainEncoder.needsConfigure(capturedFrame)) {
                const auto previousCodec = mainEncoder.encoder->codec();
                if (const auto configured = configureFor(mainEncoder, capturedFrame); !configured) {
                    std::println(stderr, "Could not set up the encoder: {}", configured.error().message);
                    captureSource->releaseFrame();
                    exitCode = 1;
                    break;
                }
                if (sessions) {
                    sessions->setOffer(offerFor(mainEncoder, options->fecPercent, cursorOverlay));
                    const auto codec = mainEncoder.encoder->codec();
                    if (previousCodec && codec != previousCodec && activeEncoder == &mainEncoder &&
                        sessions->streamingSession() != nullptr) {
                        std::println("The encoder changed to {}. Ending the session. Reconnect the tablet by hand.",
                                     codecName(codec.value_or(fm::Codec::JPEG)));
                        sessions->close(acquireTime);
                    }
                }
            }

            fm::Session* target = sessions ? sessions->streamingSession() : &developmentSession;
            if (target == nullptr) {
                captureSource->releaseFrame();
                continue;
            }
            if (activeEncoder->needsConfigure(capturedFrame)) {
                if (const auto configured = configureFor(*activeEncoder, capturedFrame); !configured) {
                    std::println(stderr, "Could not set up the encoder: {}", configured.error().message);
                    captureSource->releaseFrame();
                    exitCode = 1;
                    break;
                }
            }
            if (keyframeOwed || repeated) {
                activeEncoder->encoder->requestKeyframe();
            }
            const auto encodeStart = Clock::now();
            const bool encoded =
                    activeEncoder->encoder->encode(capturedFrame, encodedFrame) == fm::EncodeStatus::FRAME_ENCODED;
            const auto encodeEnd = Clock::now();
            captureSource->releaseFrame();
            timing.encodeStartUs = fm::toTimestampUs(encodeStart);
            timing.encodeEndUs = fm::toTimestampUs(encodeEnd);

            if (!encoded) {
                ++encodeFailures;
                timing.outcome = fm::FrameOutcome::ENCODE_FAILED;
            } else {
                encodeTimes.add(encodeEnd - encodeStart);
                encodedSizes.add(encodedFrame.data.size());
                if (encodedFrame.keyframe) {
                    lastKeyframeTime = acquireTime;
                }
                timing.outcome = fm::FrameOutcome::ENCODED;
                timing.codec = encodedFrame.codec;
                timing.keyframe = encodedFrame.keyframe;
                timing.frameBytes = static_cast<std::uint32_t>(encodedFrame.data.size());
                const auto sendStart = Clock::now();
                const auto sendResult = videoSender.send(encodedFrame, encodedFrame.codec, *target);
                const auto sendEnd = Clock::now();
                periodPackets += sendResult.packetsSent;
                periodBytes += sendResult.bytesSent;
                timing.frameId = sendResult.frameId;
                timing.packetsSent = sendResult.packetsSent;
                timing.sendStartUs = fm::toTimestampUs(sendStart);
                timing.sendEndUs = fm::toTimestampUs(sendEnd);
                if (sendResult.status == fm::FrameSendStatus::FRAME_SENT) {
                    const auto paritySent = videoSender.sendParity(options->fecPercent, *target);
                    periodPackets += paritySent.packetsSent;
                    periodBytes += paritySent.bytesSent;
                    timing.outcome = fm::FrameOutcome::SENT;
                    if (encodedFrame.keyframe) {
                        keyframeOwed = false;
                        if (sessions) {
                            sessions->keyframeSent(sendEnd);
                        }
                    }
                    lastSentWasRepeat = repeated;
                    if (repeated) {
                        lastRepeatTime = sendEnd;
                    } else {
                        lastNewFrameSentTime = sendEnd;
                        captureToSendTimes.add(sendEnd - encodedFrame.captureTime);
                    }
                } else {
                    ++sendFailures;
                    timing.outcome = fm::FrameOutcome::SEND_FAILED;
                }
            }
            if (timingLog && !repeated) {
                timingLog->record(timing);
            }
        }
    }

    if (cursorSender) {
        cursorSender->stop();
    }
    if (sessions) {
        sessions->close(Clock::now());
        const auto byeDeadline = Clock::now() + BYE_WAIT_LIMIT;
        while (sessions->closing() && Clock::now() < byeDeadline) {
            std::this_thread::sleep_for(BYE_WAIT_STEP);
            sessions->poll(Clock::now());
        }
    }
    captureSource->stop();
    std::println("Stopped after {:.1f} s", std::chrono::duration<double>(Clock::now() - startTime).count());
    if (timingLog) {
        if (const auto closed = timingLog->close(); !closed) {
            std::println(stderr, "{}", closed.error().message);
            exitCode = 1;
        }
        if (const std::uint64_t dropped = timingLog->droppedRecordCount(); dropped > 0) {
            std::println(stderr, "Warning: {} timing log rows were dropped because the log could not keep up.",
                         dropped);
        }
    }
    return exitCode;
}
