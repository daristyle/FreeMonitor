#include "HeartbeatEcho.hpp"

#include "Timestamp.hpp"

#include <limits>

namespace fm {
    void HeartbeatEcho::reset() noexcept {
        peerTimestampUs = 0;
        receivedTime = {};
        lastClockSample.reset();
    }

    void HeartbeatEcho::received(const Heartbeat& heartbeat, std::chrono::steady_clock::time_point now) noexcept {
        peerTimestampUs = heartbeat.commonHeader.timestampUs;
        receivedTime = now;
        const std::uint64_t nowUs = toTimestampUs(now);
        const std::uint64_t echoedUs = heartbeat.echoTimestampUs + heartbeat.echoDelayUs;
        if (heartbeat.echoTimestampUs != 0 && nowUs >= echoedUs) {
            const std::uint64_t peerSentUs = heartbeat.commonHeader.timestampUs;
            const std::uint64_t forwardUs = peerSentUs - heartbeat.echoDelayUs - heartbeat.echoTimestampUs;
            const std::uint64_t backwardUs = peerSentUs - nowUs;
            const std::int64_t offsetUs =
                    static_cast<std::int64_t>(forwardUs) / 2 + static_cast<std::int64_t>(backwardUs) / 2;
            lastClockSample =
                    ClockSample{.receivedTime = now,
                                .roundTripTime = std::chrono::microseconds(static_cast<std::int64_t>(nowUs - echoedUs)),
                                .peerClockOffset = std::chrono::microseconds(offsetUs)};
        }
    }

    Heartbeat HeartbeatEcho::nextHeartbeat(std::chrono::steady_clock::time_point now) const noexcept {
        Heartbeat heartbeat;
        if (peerTimestampUs == 0) {
            return heartbeat;
        }
        heartbeat.echoTimestampUs = peerTimestampUs;
        const auto delay = std::chrono::duration_cast<std::chrono::microseconds>(now - receivedTime).count();
        if (delay > 0) {
            heartbeat.echoDelayUs = delay > std::numeric_limits<std::uint32_t>::max()
                                            ? std::numeric_limits<std::uint32_t>::max()
                                            : static_cast<std::uint32_t>(delay);
        }
        return heartbeat;
    }

    std::optional<std::chrono::microseconds> HeartbeatEcho::roundTripTime() const noexcept {
        if (!lastClockSample) {
            return std::nullopt;
        }
        return lastClockSample->roundTripTime;
    }

    std::optional<ClockSample> HeartbeatEcho::clockSample() const noexcept {
        return lastClockSample;
    }
}
