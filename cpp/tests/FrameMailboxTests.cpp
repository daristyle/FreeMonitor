#include "FrameMailbox.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>

using namespace std::chrono_literals;

namespace {
    constexpr std::size_t FRAME_BYTES = 32;

    struct TestFrame {
        std::uint32_t number = 0;
    };

    using TestMailbox = fm::FrameMailbox<TestFrame>;

    void publishFilled(TestMailbox& mailbox, std::uint8_t value, std::size_t frameBytes = FRAME_BYTES) {
        std::uint8_t* bytes = mailbox.beginWrite(frameBytes);
        std::memset(bytes, value, frameBytes);
        mailbox.publish(TestFrame{value}, frameBytes);
    }

    struct Acquired {
        fm::MailboxStatus status = fm::MailboxStatus::NO_NEW_FRAME;
        TestFrame frame;
        std::span<const std::uint8_t> bytes;
    };

    Acquired acquire(TestMailbox& mailbox, std::chrono::milliseconds timeout) {
        Acquired acquired;
        acquired.status = mailbox.acquire(acquired.frame, acquired.bytes, timeout);
        return acquired;
    }
}

TEST_CASE("acquire times out with NO_NEW_FRAME when nothing was published") {
    TestMailbox mailbox;
    CHECK(acquire(mailbox, 0ms).status == fm::MailboxStatus::NO_NEW_FRAME);
    CHECK(acquire(mailbox, 10ms).status == fm::MailboxStatus::NO_NEW_FRAME);
}

TEST_CASE("a published frame is delivered once with its description and exact size") {
    TestMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 7, FRAME_BYTES - 5);

    const auto acquired = acquire(mailbox, 0ms);
    REQUIRE(acquired.status == fm::MailboxStatus::FRAME_READY);
    CHECK(acquired.frame.number == 7);
    REQUIRE(acquired.bytes.size() == FRAME_BYTES - 5);
    CHECK(acquired.bytes.front() == 7);
    CHECK(acquired.bytes.back() == 7);
    mailbox.release();

    CHECK(acquire(mailbox, 0ms).status == fm::MailboxStatus::NO_NEW_FRAME);
}

TEST_CASE("an unread frame is replaced by a newer one and counted as dropped") {
    TestMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 1);
    publishFilled(mailbox, 2);
    publishFilled(mailbox, 3);

    const auto acquired = acquire(mailbox, 0ms);
    REQUIRE(acquired.status == fm::MailboxStatus::FRAME_READY);
    CHECK(acquired.frame.number == 3);
    CHECK(acquired.bytes.front() == 3);
    CHECK(mailbox.droppedFrameCount() == 2);
    mailbox.release();
}

TEST_CASE("the producer never writes into the frame the consumer holds") {
    TestMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 1);

    const auto held = acquire(mailbox, 0ms);
    REQUIRE(held.status == fm::MailboxStatus::FRAME_READY);
    for (std::uint8_t value = 2; value < 20; ++value) {
        publishFilled(mailbox, value);
    }
    CHECK(held.bytes.front() == 1);
    CHECK(held.bytes.back() == 1);
    mailbox.release();

    const auto next = acquire(mailbox, 0ms);
    REQUIRE(next.status == fm::MailboxStatus::FRAME_READY);
    CHECK(next.frame.number == 19);
    CHECK(next.bytes.front() == 19);
    mailbox.release();
}

TEST_CASE("steady state reuses the reserved buffers") {
    TestMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    const std::uint8_t* first = mailbox.beginWrite(FRAME_BYTES);
    for (int i = 0; i < 6; ++i) {
        publishFilled(mailbox, 1);
        REQUIRE(acquire(mailbox, 0ms).status == fm::MailboxStatus::FRAME_READY);
        mailbox.release();
    }
    bool seenFirstAgain = false;
    for (int i = 0; i < 3; ++i) {
        seenFirstAgain = seenFirstAgain || mailbox.beginWrite(FRAME_BYTES) == first;
        publishFilled(mailbox, 1);
    }
    CHECK(seenFirstAgain);
}

TEST_CASE("close wakes a waiting consumer with CLOSED") {
    TestMailbox mailbox;
    std::thread closer([&mailbox] {
        std::this_thread::sleep_for(20ms);
        mailbox.close();
    });

    const auto started = std::chrono::steady_clock::now();
    CHECK(acquire(mailbox, 5s).status == fm::MailboxStatus::CLOSED);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    closer.join();
}

TEST_CASE("a frame published before close is still delivered") {
    TestMailbox mailbox;
    mailbox.reserve(FRAME_BYTES);
    publishFilled(mailbox, 9);
    mailbox.close();

    const auto acquired = acquire(mailbox, 0ms);
    REQUIRE(acquired.status == fm::MailboxStatus::FRAME_READY);
    CHECK(acquired.frame.number == 9);
    mailbox.release();
    CHECK(acquire(mailbox, 0ms).status == fm::MailboxStatus::CLOSED);
}

TEST_CASE("reset reopens a closed mailbox") {
    TestMailbox mailbox;
    mailbox.close();
    mailbox.reset();
    CHECK(acquire(mailbox, 0ms).status == fm::MailboxStatus::NO_NEW_FRAME);
}

TEST_CASE("producer and consumer threads never see a torn frame") {
    TestMailbox mailbox;
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
        const auto acquired = acquire(mailbox, 10ms);
        if (acquired.status != fm::MailboxStatus::FRAME_READY) {
            continue;
        }
        bool uniform = acquired.frame.number == acquired.bytes.front();
        for (const std::uint8_t byte : acquired.bytes) {
            uniform = uniform && byte == acquired.bytes.front();
        }
        CHECK(uniform);
        ++framesChecked;
        mailbox.release();
    }
    stop.store(true);
    producer.join();
    CHECK(framesChecked > 0);
}
