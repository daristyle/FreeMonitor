#pragma once

#include "Error.hpp"
#include "HeartbeatEcho.hpp"
#include "Packets.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>
#include <variant>

namespace fm {
    constexpr std::size_t FRAME_TIMING_LOG_CAPACITY = 1024;
    constexpr std::chrono::milliseconds FRAME_TIMING_FLUSH_INTERVAL{500};

    enum class FrameOutcome : std::uint8_t {
        SENT,
        ENCODED,
        ENCODE_FAILED,
        SEND_FAILED,
    };

    struct FrameTiming {
        std::optional<std::uint32_t> frameId = std::nullopt;
        FrameOutcome outcome = FrameOutcome::SENT;
        Codec codec = Codec::JPEG;
        bool keyframe = false;
        std::uint32_t frameBytes = 0;
        std::uint16_t packetsSent = 0;
        std::uint64_t captureUs = 0;
        std::uint64_t acquireUs = 0;
        std::uint64_t encodeStartUs = 0;
        std::uint64_t encodeEndUs = 0;
        std::uint64_t sendStartUs = 0;
        std::uint64_t sendEndUs = 0;
    };

    std::filesystem::path clockLogPath(const std::filesystem::path& timingLogPath);

    class FrameTimingLog {
    private:
        using Record = std::variant<FrameTiming, ClockSample>;

        std::ofstream file;
        std::ofstream clockFile;
        std::chrono::milliseconds flushInterval;
        std::array<Record, FRAME_TIMING_LOG_CAPACITY> records{};
        std::atomic<std::uint64_t> head{0};
        std::atomic<std::uint64_t> tail{0};
        std::atomic<std::uint64_t> droppedRecords{0};
        std::binary_semaphore stopSignal{0};
        std::thread writerThread;

        FrameTimingLog(std::ofstream file, std::ofstream clockFile, std::chrono::milliseconds flushInterval);
        void push(const Record& record) noexcept;
        void run();
        void drain();

    public:
        static Result<std::unique_ptr<FrameTimingLog>>
        open(const std::filesystem::path& path, std::chrono::milliseconds flushInterval = FRAME_TIMING_FLUSH_INTERVAL);
        ~FrameTimingLog();

        FrameTimingLog(const FrameTimingLog&) = delete;
        FrameTimingLog& operator=(const FrameTimingLog&) = delete;

        void record(const FrameTiming& timing) noexcept;
        void record(const ClockSample& sample) noexcept;
        Result<> close();
        std::uint64_t droppedRecordCount() const noexcept;
    };
}
