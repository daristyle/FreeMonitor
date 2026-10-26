#include "FrameTimingLog.hpp"
#include "TemporaryDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::chrono::milliseconds NEVER_FLUSH = 1h;
    constexpr std::chrono::milliseconds QUICK_FLUSH = 10ms;
    constexpr std::chrono::milliseconds WAIT_TIMEOUT = 2s;
    constexpr const char* HEADER_COMMENT = "# freemonitor-timing 1 host";
    constexpr const char* HEADER_COLUMNS = "frameId,outcome,codec,keyframe,frameBytes,packetsSent,captureUs,acquireUs,"
                                           "encodeStartUs,encodeEndUs,sendStartUs,sendEndUs";

    std::vector<std::string> readLines(const std::filesystem::path& path) {
        std::ifstream file(path);
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) {
            lines.push_back(line);
        }
        return lines;
    }

    std::unique_ptr<fm::FrameTimingLog> openLog(const std::filesystem::path& path,
                                                std::chrono::milliseconds flushInterval) {
        auto log = fm::FrameTimingLog::open(path, flushInterval);
        REQUIRE(log);
        return std::move(*log);
    }
}

TEST_CASE("a timing log starts with its header and writes one row per frame") {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "host-timing.csv";
    auto log = openLog(path, NEVER_FLUSH);

    log->record(fm::FrameTiming{.frameId = 7,
                                .outcome = fm::FrameOutcome::SENT,
                                .codec = fm::Codec::JPEG,
                                .keyframe = true,
                                .frameBytes = 1234,
                                .packetsSent = 2,
                                .captureUs = 100,
                                .acquireUs = 110,
                                .encodeStartUs = 120,
                                .encodeEndUs = 130,
                                .sendStartUs = 140,
                                .sendEndUs = 150});
    log->record(fm::FrameTiming{.outcome = fm::FrameOutcome::ENCODE_FAILED,
                                .captureUs = 200,
                                .acquireUs = 210,
                                .encodeStartUs = 220,
                                .encodeEndUs = 230});
    REQUIRE(log->close());
    CHECK(log->close());

    CHECK(readLines(path) == std::vector<std::string>{HEADER_COMMENT, HEADER_COLUMNS,
                                                      "7,SENT,JPEG,1,1234,2,100,110,120,130,140,150",
                                                      ",ENCODE_FAILED,JPEG,0,0,0,200,210,220,230,,"});
    CHECK(log->droppedRecordCount() == 0);
}

TEST_CASE("a full timing log drops records instead of blocking") {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "host-timing.csv";
    auto log = openLog(path, NEVER_FLUSH);

    for (std::size_t i = 0; i < fm::FRAME_TIMING_LOG_CAPACITY + 3; ++i) {
        log->record(fm::FrameTiming{.frameId = static_cast<std::uint32_t>(i), .captureUs = 1});
    }
    CHECK(log->droppedRecordCount() == 3);
    REQUIRE(log->close());

    const auto lines = readLines(path);
    REQUIRE(lines.size() == 2 + fm::FRAME_TIMING_LOG_CAPACITY);
    CHECK(lines[2].starts_with("0,"));
    CHECK(lines.back().starts_with(std::to_string(fm::FRAME_TIMING_LOG_CAPACITY - 1) + ","));
}

TEST_CASE("the writer thread flushes records while the log is open") {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "host-timing.csv";
    auto log = openLog(path, QUICK_FLUSH);

    log->record(fm::FrameTiming{.frameId = 1, .captureUs = 5});
    const auto deadline = std::chrono::steady_clock::now() + WAIT_TIMEOUT;
    while (readLines(path).size() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(readLines(path).size() == 3);
    REQUIRE(log->close());
}

TEST_CASE("a timing log in a missing directory cannot be opened") {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "missing" / "host-timing.csv";
    const auto log = fm::FrameTimingLog::open(path);
    REQUIRE_FALSE(log);
    CHECK(log.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    CHECK(log.error().message.find("host-timing.csv") != std::string::npos);
}
