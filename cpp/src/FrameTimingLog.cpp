#include "FrameTimingLog.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fm {
    namespace {
        constexpr std::string_view HEADER = "# freemonitor-timing 1 host\n"
                                            "frameId,outcome,codec,keyframe,frameBytes,packetsSent,captureUs,acquireUs,"
                                            "encodeStartUs,encodeEndUs,sendStartUs,sendEndUs\n";

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
    }

    FrameTimingLog::FrameTimingLog(std::ofstream file, std::chrono::milliseconds flushInterval)
        : file(std::move(file)), flushInterval(flushInterval) {
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
        return std::unique_ptr<FrameTimingLog>(new FrameTimingLog(std::move(file), flushInterval));
    }

    FrameTimingLog::~FrameTimingLog() {
        static_cast<void>(close());
    }

    void FrameTimingLog::record(const FrameTiming& timing) noexcept {
        const std::uint64_t writeIndex = head.load(std::memory_order_relaxed);
        if (writeIndex - tail.load(std::memory_order_acquire) == FRAME_TIMING_LOG_CAPACITY) {
            droppedRecords.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        records[writeIndex % FRAME_TIMING_LOG_CAPACITY] = timing;
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
        if (file.fail()) {
            return std::unexpected(Error{ErrorCode::DEVICE_FAILURE, "Could not write the timing log"});
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
        for (; index != end; ++index) {
            text += formatRow(records[index % FRAME_TIMING_LOG_CAPACITY]);
        }
        tail.store(end, std::memory_order_release);
        file << text;
        file.flush();
    }
}
