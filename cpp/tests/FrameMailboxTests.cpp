#include "FrameMailbox.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;

namespace {
    constexpr std::uint32_t FRAME_WIDTH = 4;
    constexpr std::uint32_t FRAME_HEIGHT = 2;
    constexpr std::size_t FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 4;

    void publishFilled(fm::FrameMailbox& mailbox, std::uint8_t value) {
        std::uint8_t* pixels = mailbox.beginWrite(FRAME_BYTES);
        std::memset(pixels, value, FRAME_BYTES);
        fm::CapturedFrame description;
        description.width = FRAME_WIDTH;
        description.height = FRAME_HEIGHT;
        description.stride = FRAME_WIDTH * 4;
        description.captureTime = std::chrono::steady_clock::now();
        mailbox.publish(description);
    }
}

TEST_CASE("acquire times out with NO_NEW_FRAME when nothing was published") {
    fm::FrameMailbox mailbox;
    fm::CapturedFrame frame;
    CHECK(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::NO_NEW_FRAME);
    CHECK(mailbox.acquire(frame, 10ms) == fm::CaptureStatus::NO_NEW_FRAME);
}

TEST_CASE("a published frame is delivered once with its description") {
    fm::FrameMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 7);

    fm::CapturedFrame frame;
    REQUIRE(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    CHECK(frame.width == FRAME_WIDTH);
    CHECK(frame.height == FRAME_HEIGHT);
    CHECK(frame.stride == FRAME_WIDTH * 4);
    REQUIRE(frame.pixels != nullptr);
    CHECK(frame.pixels[0] == 7);
    CHECK(frame.pixels[FRAME_BYTES - 1] == 7);
    mailbox.release();

    CHECK(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::NO_NEW_FRAME);
}

TEST_CASE("an unread frame is replaced by a newer one and counted as dropped") {
    fm::FrameMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 1);
    publishFilled(mailbox, 2);
    publishFilled(mailbox, 3);

    fm::CapturedFrame frame;
    REQUIRE(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    CHECK(frame.pixels[0] == 3);
    CHECK(mailbox.droppedFrameCount() == 2);
    mailbox.release();
}

TEST_CASE("the producer never writes into the frame the consumer holds") {
    fm::FrameMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 1);

    fm::CapturedFrame held;
    REQUIRE(mailbox.acquire(held, 0ms) == fm::CaptureStatus::FRAME_READY);
    for (std::uint8_t value = 2; value < 20; ++value) {
        publishFilled(mailbox, value);
    }
    CHECK(held.pixels[0] == 1);
    CHECK(held.pixels[FRAME_BYTES - 1] == 1);
    mailbox.release();

    fm::CapturedFrame next;
    REQUIRE(mailbox.acquire(next, 0ms) == fm::CaptureStatus::FRAME_READY);
    CHECK(next.pixels[0] == 19);
    mailbox.release();
}

TEST_CASE("steady state reuses the reserved buffers") {
    fm::FrameMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    const std::uint8_t* first = mailbox.beginWrite(FRAME_BYTES);
    for (int i = 0; i < 6; ++i) {
        publishFilled(mailbox, 1);
        fm::CapturedFrame frame;
        REQUIRE(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
        mailbox.release();
    }
    bool seenFirstAgain = false;
    for (int i = 0; i < 3; ++i) {
        seenFirstAgain = seenFirstAgain || mailbox.beginWrite(FRAME_BYTES) == first;
        publishFilled(mailbox, 1);
    }
    CHECK(seenFirstAgain);
}

TEST_CASE("close wakes a waiting consumer with CAPTURE_LOST") {
    fm::FrameMailbox mailbox;
    std::thread closer([&mailbox] {
        std::this_thread::sleep_for(20ms);
        mailbox.close();
    });

    fm::CapturedFrame frame;
    const auto started = std::chrono::steady_clock::now();
    CHECK(mailbox.acquire(frame, 5s) == fm::CaptureStatus::CAPTURE_LOST);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    closer.join();
}

TEST_CASE("a frame published before close is still delivered") {
    fm::FrameMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 9);
    mailbox.close();

    fm::CapturedFrame frame;
    REQUIRE(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::FRAME_READY);
    CHECK(frame.pixels[0] == 9);
    mailbox.release();
    CHECK(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::CAPTURE_LOST);
}

TEST_CASE("reset reopens a closed mailbox") {
    fm::FrameMailbox mailbox;
    mailbox.close();
    mailbox.reset();

    fm::CapturedFrame frame;
    CHECK(mailbox.acquire(frame, 0ms) == fm::CaptureStatus::NO_NEW_FRAME);
}

TEST_CASE("producer and consumer threads never see a torn frame") {
    fm::FrameMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    std::atomic<bool> stop = false;

    std::thread producer([&] {
        std::uint8_t value = 0;
        while (!stop.load()) {
            publishFilled(mailbox, ++value);
        }
        mailbox.close();
    });

    int framesChecked = 0;
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < deadline) {
        fm::CapturedFrame frame;
        if (mailbox.acquire(frame, 10ms) != fm::CaptureStatus::FRAME_READY) {
            continue;
        }
        bool uniform = true;
        for (std::size_t i = 1; i < FRAME_BYTES; ++i) {
            uniform = uniform && frame.pixels[i] == frame.pixels[0];
        }
        CHECK(uniform);
        ++framesChecked;
        mailbox.release();
    }
    stop.store(true);
    producer.join();
    CHECK(framesChecked > 0);
}
