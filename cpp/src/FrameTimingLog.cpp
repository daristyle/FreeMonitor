#include "FrameTimingLog.hpp"

#include "Timestamp.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fm {
    namespace {
        constexpr std::string_view HEADER = "# freemonitor-timing 1 host\n"
                                            "frameId,outcome,codec,keyframe,frameBytes,packetsSent,captureUs,acquireUs,"
                                            "encodeStartUs,encodeEndUs,sendStartUs,sendEndUs\n";
        constexpr std::string_view CLOCK_HEADER = "# freemonitor-clock 1 host\n"
                                                  "receivedUs,offsetUs,rttUs\n";

        std::string_view outcomeName(FrameOutcome outcome) noexcept {
            switch (outcome) {
                case FrameOutcome::SENT:
                    return "SENT";
                case FrameOutcome::ENCODED:
                    return "ENCODED";
                case FrameOutcome::ENCODE_FAILED:
                    return "ENCODE_FAILED";
                case FrameOutcome::SEND_FAILED:
                    return "SEND_FAILED";
            }
            return "";
        }

        std::string_view codecName(Codec codec) noexcept {
            switch (codec) {
                case Codec::JPEG:
                    return "JPEG";
                case Codec::H264:
                    return "H264";
            }
            return "";
        }

        std::string timestampField(std::uint64_t timestampUs) {
            return timestampUs == 0 ? std::string() : std::to_string(timestampUs);
        }

        std::string formatRow(const FrameTiming& timing) {
            return std::format("{},{},{},{},{},{},{},{},{},{},{},{}\n",
                               timing.frameId ? std::to_string(*timing.frameId) : std::string(),
                               outcomeName(timing.outcome), codecName(timing.codec), timing.keyframe ? 1 : 0,
                               timing.frameBytes, timing.packetsSent, timestampField(timing.captureUs),
                               timestampField(timing.acquireUs), timestampField(timing.encodeStartUs),
                               timestampField(timing.encodeEndUs), timestampField(timing.sendStartUs),
                               timestampField(timing.sendEndUs));
        }

        std::string formatRow(const ClockSample& sample) {
            return std::format("{},{},{}\n", toTimestampUs(sample.receivedTime), sample.peerClockOffset.count(),
                               sample.roundTripTime.count());
        }
    }

    std::filesystem::path clockLogPath(const std::filesystem::path& timingLogPath) {
        return std::filesystem::path(timingLogPath).replace_extension(".clock.csv");
    }

    FrameTimingLog::FrameTimingLog(std::ofstream file, std::ofstream clockFile, std::chrono::milliseconds flushInterval)
        : file(std::move(file)), clockFile(std::move(clockFile)), flushInterval(flushInterval) {
        writerThread = std::thread([this] { run(); });
    }

    Result<std::unique_ptr<FrameTimingLog>> FrameTimingLog::open(const std::filesystem::path& path,
                                                                 std::chrono::milliseconds flushInterval) {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        file << HEADER;
        file.flush();
        if (!file) {
            return std::unexpected(
                    Error{ErrorCode::INVALID_ARGUMENT, "Could not write the timing log " + path.string()});
        }
        const std::filesystem::path clockPath = clockLogPath(path);
        std::ofstream clockFile(clockPath, std::ios::out | std::ios::trunc);
        clockFile << CLOCK_HEADER;
        clockFile.flush();
        if (!clockFile) {
            return std::unexpected(
                    Error{ErrorCode::INVALID_ARGUMENT, "Could not write the clock log " + clockPath.string()});
        }
        return std::unique_ptr<FrameTimingLog>(
                new FrameTimingLog(std::move(file), std::move(clockFile), flushInterval));
    }

    FrameTimingLog::~FrameTimingLog() {
        static_cast<void>(close());
    }

    void FrameTimingLog::record(const FrameTiming& timing) noexcept {
        push(timing);
    }

    void FrameTimingLog::record(const ClockSample& sample) noexcept {
        push(sample);
    }

    void FrameTimingLog::push(const Record& record) noexcept {
        const std::uint64_t writeIndex = head.load(std::memory_order_relaxed);
        if (writeIndex - tail.load(std::memory_order_acquire) == FRAME_TIMING_LOG_CAPACITY) {
            droppedRecords.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        records[writeIndex % FRAME_TIMING_LOG_CAPACITY] = record;
        head.store(writeIndex + 1, std::memory_order_release);
    }

    Result<> FrameTimingLog::close() {
        if (writerThread.joinable()) {
            stopSignal.release();
            writerThread.join();
        }
        if (!file.is_open()) {
            return {};
        }
        drain();
        file.close();
        clockFile.close();
        if (file.fail()) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not write the timing log"});
        }
        if (clockFile.fail()) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not write the clock log"});
        }
        return {};
    }

    std::uint64_t FrameTimingLog::droppedRecordCount() const noexcept {
        return droppedRecords.load(std::memory_order_relaxed);
    }

    void FrameTimingLog::run() {
        bool stopping = false;
        while (!stopping) {
            stopping = stopSignal.try_acquire_for(flushInterval);
            drain();
        }
    }

    void FrameTimingLog::drain() {
        const std::uint64_t end = head.load(std::memory_order_acquire);
        std::uint64_t index = tail.load(std::memory_order_relaxed);
        if (index == end) {
            return;
        }
        std::string text;
        std::string clockText;
        for (; index != end; ++index) {
            const Record& record = records[index % FRAME_TIMING_LOG_CAPACITY];
            if (const auto* timing = std::get_if<FrameTiming>(&record)) {
                text += formatRow(*timing);
            } else {
                clockText += formatRow(std::get<ClockSample>(record));
            }
        }
        tail.store(end, std::memory_order_release);
        file << text;
        file.flush();
        clockFile << clockText;
        clockFile.flush();
    }
}
