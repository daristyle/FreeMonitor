#include "Fragmenter.hpp"
#include "Reassembler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numeric>
#include <random>
#include <vector>

using namespace std::chrono_literals;

namespace {
    using Clock = std::chrono::steady_clock;
    using Status = fm::FragmentStatus;

    constexpr std::uint32_t SESSION_ID = 0x11223344;
    constexpr std::size_t THREE_FRAGMENTS = 3 * fm::MAX_FRAGMENT_PAYLOAD - 100;
    const Clock::time_point START = Clock::time_point{} + 1h;

    struct TestFrame {
        std::vector<std::uint8_t> data;
        std::vector<std::vector<std::uint8_t>> packets;
        std::vector<fm::VideoFragment> fragments;
    };

    TestFrame makeTestFrame(std::size_t size, std::uint32_t frameId, fm::Codec codec = fm::Codec::JPEG,
                            bool keyframe = true, std::uint64_t captureTimestampUs = 1234) {
        TestFrame frame;
        frame.data.resize(size);
        for (std::size_t index = 0; index < size; ++index) {
            frame.data[index] = static_cast<std::uint8_t>((index * 7 + frameId) % 251);
        }
        fm::Fragmenter fragmenter;
        fragmenter.begin(frame.data, frameId, codec, keyframe, captureTimestampUs);
        for (std::uint16_t index = 0; index < fragmenter.fragmentCount(); ++index) {
            std::vector<std::uint8_t> packet(fm::MAX_PACKET_SIZE);
            packet.resize(fragmenter.write(index, SESSION_ID, index, 0, packet));
            frame.packets.push_back(std::move(packet));
        }
        for (const std::vector<std::uint8_t>& packet : frame.packets) {
            fm::VideoFragment fragment;
            fragment.read(packet);
            frame.fragments.push_back(fragment);
        }
        return frame;
    }

    std::vector<std::size_t> shuffledOrder(const TestFrame& frame) {
        std::vector<std::size_t> order(frame.fragments.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::mt19937 random(42);
        std::ranges::shuffle(order, random);
        return order;
    }

    Status feed(fm::Reassembler& reassembler, const TestFrame& frame, std::initializer_list<std::size_t> indices,
                Clock::time_point now) {
        Status status = Status::INVALID;
        for (const std::size_t index : indices) {
            status = reassembler.accept(frame.fragments[index], now);
        }
        return status;
    }

    Status feedAll(fm::Reassembler& reassembler, const TestFrame& frame, Clock::time_point now) {
        Status status = Status::INVALID;
        for (const fm::VideoFragment& fragment : frame.fragments) {
            status = reassembler.accept(fragment, now);
        }
        return status;
    }

    bool holds(const fm::Reassembler& reassembler, const TestFrame& frame) {
        return std::ranges::equal(reassembler.completedFrame().data, frame.data);
    }
}

TEST_CASE("a single-fragment frame is complete at once") {
    const TestFrame frame = makeTestFrame(100, 7, fm::Codec::H264, true, 0x0102030405060708);
    fm::Reassembler reassembler;
    REQUIRE(reassembler.accept(frame.fragments[0], START) == Status::FRAME_COMPLETE);

    const fm::ReassembledFrame completed = reassembler.completedFrame();
    CHECK(std::ranges::equal(completed.data, frame.data));
    CHECK(completed.frameId == 7);
    CHECK(completed.codec == fm::Codec::H264);
    CHECK(completed.keyframe);
    CHECK(completed.captureTimestampUs == 0x0102030405060708);
    CHECK(reassembler.framesCompleted() == 1);
    CHECK(reassembler.framesDropped() == 0);
}

TEST_CASE("a completed frame carries the arrival times of its first and last fragment") {
    const TestFrame first = makeTestFrame(THREE_FRAGMENTS, 9);
    const TestFrame second = makeTestFrame(THREE_FRAGMENTS, 10);
    fm::Reassembler reassembler;

    REQUIRE(reassembler.accept(first.fragments[2], START) == Status::ACCEPTED);
    REQUIRE(reassembler.accept(first.fragments[0], START + 3ms) == Status::ACCEPTED);
    REQUIRE(reassembler.accept(first.fragments[0], START + 4ms) == Status::DUPLICATE);
    REQUIRE(reassembler.accept(first.fragments[1], START + 5ms) == Status::FRAME_COMPLETE);
    CHECK(reassembler.completedFrame().firstFragmentTime == START);
    CHECK(reassembler.completedFrame().completeTime == START + 5ms);

    REQUIRE(feed(reassembler, second, {0, 1}, START + 10ms) == Status::ACCEPTED);
    REQUIRE(feed(reassembler, second, {2}, START + 12ms) == Status::FRAME_COMPLETE);
    CHECK(reassembler.completedFrame().firstFragmentTime == START + 10ms);
    CHECK(reassembler.completedFrame().completeTime == START + 12ms);
}

TEST_CASE("fragments in order rebuild the frame") {
    const TestFrame frame = makeTestFrame(50'000, 1);
    fm::Reassembler reassembler;
    for (std::size_t index = 0; index + 1 < frame.fragments.size(); ++index) {
        REQUIRE(reassembler.accept(frame.fragments[index], START) == Status::ACCEPTED);
    }
    REQUIRE(reassembler.accept(frame.fragments.back(), START) == Status::FRAME_COMPLETE);
    CHECK(holds(reassembler, frame));
}

TEST_CASE("out-of-order fragments rebuild the frame") {
    const TestFrame frame = makeTestFrame(50'000, 1);
    std::vector<std::size_t> order(frame.fragments.size());
    std::iota(order.begin(), order.end(), std::size_t{0});

    SECTION("reversed") {
        std::ranges::reverse(order);
    }
    SECTION("shuffled") {
        order = shuffledOrder(frame);
    }

    fm::Reassembler reassembler;
    for (std::size_t position = 0; position + 1 < order.size(); ++position) {
        REQUIRE(reassembler.accept(frame.fragments[order[position]], START) == Status::ACCEPTED);
    }
    REQUIRE(reassembler.accept(frame.fragments[order.back()], START) == Status::FRAME_COMPLETE);
    CHECK(holds(reassembler, frame));
}

TEST_CASE("duplicate fragments are ignored and complete the frame only once") {
    const TestFrame frame = makeTestFrame(THREE_FRAGMENTS, 1);
    fm::Reassembler reassembler;
    CHECK(feed(reassembler, frame, {0}, START) == Status::ACCEPTED);
    CHECK(feed(reassembler, frame, {0}, START) == Status::DUPLICATE);
    CHECK(feed(reassembler, frame, {1}, START) == Status::ACCEPTED);
    CHECK(feed(reassembler, frame, {1}, START) == Status::DUPLICATE);
    CHECK(feed(reassembler, frame, {0}, START) == Status::DUPLICATE);
    REQUIRE(feed(reassembler, frame, {2}, START) == Status::FRAME_COMPLETE);
    CHECK(holds(reassembler, frame));

    CHECK(feed(reassembler, frame, {2}, START) == Status::TOO_OLD);
    CHECK(reassembler.framesCompleted() == 1);
}

TEST_CASE("a frame with a missing fragment is dropped after FRAGMENT_TIMEOUT_MS") {
    const TestFrame frame = makeTestFrame(THREE_FRAGMENTS, 1);
    const TestFrame next = makeTestFrame(100, 2);
    fm::Reassembler reassembler;
    REQUIRE(feed(reassembler, frame, {0, 2}, START) == Status::ACCEPTED);

    reassembler.expire(START + 99ms);
    CHECK(reassembler.framesDropped() == 0);
    reassembler.expire(START + 100ms);
    CHECK(reassembler.framesDropped() == 1);

    CHECK(feed(reassembler, frame, {1}, START + 100ms) == Status::ACCEPTED);
    CHECK(feedAll(reassembler, next, START + 100ms) == Status::FRAME_COMPLETE);
    CHECK(reassembler.framesCompleted() == 1);
    CHECK(reassembler.framesDropped() == 2);
}

TEST_CASE("a new fragment restarts the timeout, a duplicate does not") {
    const TestFrame frame = makeTestFrame(THREE_FRAGMENTS, 1);
    fm::Reassembler reassembler;
    REQUIRE(feed(reassembler, frame, {0}, START) == Status::ACCEPTED);

    SECTION("new fragment") {
        REQUIRE(feed(reassembler, frame, {1}, START + 90ms) == Status::ACCEPTED);
        reassembler.expire(START + 180ms);
        CHECK(reassembler.framesDropped() == 0);
        CHECK(feed(reassembler, frame, {2}, START + 180ms) == Status::FRAME_COMPLETE);
    }
    SECTION("duplicate") {
        REQUIRE(feed(reassembler, frame, {0}, START + 90ms) == Status::DUPLICATE);
        reassembler.expire(START + 100ms);
        CHECK(reassembler.framesDropped() == 1);
    }
}

TEST_CASE("a large frame that keeps arriving never times out") {
    const TestFrame frame = makeTestFrame(fm::MAX_FRAME_SIZE, 1);
    fm::Reassembler reassembler;
    Clock::time_point now = START;
    Status status = Status::INVALID;
    for (const fm::VideoFragment& fragment : frame.fragments) {
        status = reassembler.accept(fragment, now);
        now += 50ms;
    }
    CHECK(status == Status::FRAME_COMPLETE);
    CHECK(reassembler.framesDropped() == 0);
    CHECK(holds(reassembler, frame));
}

TEST_CASE("frameId order survives the wrap from 2^32 - 1 to 0") {
    const TestFrame last = makeTestFrame(100, 0xFFFFFFFF);
    const TestFrame first = makeTestFrame(100, 0);
    fm::Reassembler reassembler;
    REQUIRE(feedAll(reassembler, last, START) == Status::FRAME_COMPLETE);
    CHECK(feedAll(reassembler, first, START) == Status::FRAME_COMPLETE);
    CHECK(reassembler.completedFrame().frameId == 0);
    CHECK(feedAll(reassembler, last, START) == Status::TOO_OLD);
}

TEST_CASE("fragments of the last delivered frame or older ones are too old") {
    const TestFrame older = makeTestFrame(100, 9);
    const TestFrame delivered = makeTestFrame(100, 10);
    const TestFrame newer = makeTestFrame(100, 11);
    fm::Reassembler reassembler;
    REQUIRE(feedAll(reassembler, delivered, START) == Status::FRAME_COMPLETE);
    CHECK(feedAll(reassembler, delivered, START) == Status::TOO_OLD);
    CHECK(feedAll(reassembler, older, START) == Status::TOO_OLD);
    CHECK(feedAll(reassembler, newer, START) == Status::FRAME_COMPLETE);
}

TEST_CASE("delivering a frame discards the older incomplete frames") {
    const TestFrame first = makeTestFrame(2 * fm::MAX_FRAGMENT_PAYLOAD, 1);
    const TestFrame second = makeTestFrame(2 * fm::MAX_FRAGMENT_PAYLOAD, 2);
    const TestFrame third = makeTestFrame(100, 3);
    fm::Reassembler reassembler;
    REQUIRE(feed(reassembler, first, {0}, START) == Status::ACCEPTED);
    REQUIRE(feed(reassembler, second, {0}, START) == Status::ACCEPTED);
    REQUIRE(feedAll(reassembler, third, START) == Status::FRAME_COMPLETE);

    CHECK(reassembler.framesDropped() == 2);
    CHECK(feed(reassembler, first, {1}, START) == Status::TOO_OLD);
    CHECK(feed(reassembler, second, {1}, START) == Status::TOO_OLD);
}

TEST_CASE("when all slots are full") {
    const TestFrame older = makeTestFrame(2 * fm::MAX_FRAGMENT_PAYLOAD, 1);
    std::vector<TestFrame> frames;
    for (std::uint32_t frameId = 2; frameId <= fm::MAX_INCOMPLETE_FRAMES + 2; ++frameId) {
        frames.push_back(makeTestFrame(2 * fm::MAX_FRAGMENT_PAYLOAD, frameId));
    }
    fm::Reassembler reassembler;
    for (std::size_t index = 0; index < fm::MAX_INCOMPLETE_FRAMES; ++index) {
        REQUIRE(feed(reassembler, frames[index], {0}, START) == Status::ACCEPTED);
    }

    SECTION("a newer frame evicts the oldest incomplete frame") {
        CHECK(feed(reassembler, frames.back(), {0}, START) == Status::ACCEPTED);
        CHECK(reassembler.framesDropped() == 1);
        CHECK(feed(reassembler, frames[0], {1}, START) == Status::TOO_OLD);
        CHECK(feed(reassembler, frames[1], {1}, START) == Status::FRAME_COMPLETE);
    }
    SECTION("a frame older than all of them is dropped") {
        CHECK(feed(reassembler, older, {0}, START) == Status::TOO_OLD);
        CHECK(reassembler.framesDropped() == 0);
        CHECK(feed(reassembler, frames[0], {1}, START) == Status::FRAME_COMPLETE);
    }
}

TEST_CASE("invalid fragments are dropped without changing any state") {
    const TestFrame frame = makeTestFrame(THREE_FRAGMENTS, 5);
    const std::vector<std::uint8_t> oversizedPayload(fm::MAX_FRAGMENT_PAYLOAD + 1);
    fm::Reassembler reassembler;
    REQUIRE(feed(reassembler, frame, {0}, START) == Status::ACCEPTED);
    fm::VideoFragment bad = frame.fragments[1];

    SECTION("fragCount is 0") {
        bad.fragCount = 0;
    }
    SECTION("fragIndex is not below fragCount") {
        bad.fragIndex = 3;
    }
    SECTION("frameSize is 0") {
        bad.frameSize = 0;
    }
    SECTION("frameSize is over MAX_FRAME_SIZE") {
        bad.frameSize = fm::MAX_FRAME_SIZE + 1;
    }
    SECTION("fragCount does not match frameSize") {
        bad.fragCount = 4;
    }
    SECTION("payload is too short") {
        bad.payload = bad.payload.first(bad.payload.size() - 1);
    }
    SECTION("payload is too long") {
        bad.payload = oversizedPayload;
    }
    SECTION("last fragment has the wrong length") {
        bad = frame.fragments[2];
        bad.payload = bad.payload.first(bad.payload.size() - 1);
    }
    SECTION("codec is unknown") {
        bad.codec = 2;
    }
    SECTION("a reserved flag bit is set") {
        bad.flags = static_cast<std::uint8_t>(bad.flags | 0x02);
    }
    SECTION("the reserved field is not 0") {
        bad.reserved = 1;
    }
    SECTION("disagrees with an earlier fragment on frameSize") {
        bad.frameSize -= 1;
    }
    SECTION("disagrees with an earlier fragment on codec") {
        bad.codec = static_cast<std::uint8_t>(fm::Codec::H264);
    }
    SECTION("disagrees with an earlier fragment on flags") {
        bad.flags = 0;
    }
    SECTION("disagrees with an earlier fragment on captureTimestampUs") {
        bad.captureTimestampUs += 1;
    }

    CHECK(reassembler.accept(bad, START + 50ms) == Status::INVALID);
    CHECK(feed(reassembler, frame, {1, 2}, START + 50ms) == Status::FRAME_COMPLETE);
    CHECK(holds(reassembler, frame));
    CHECK(reassembler.framesDropped() == 0);
}

TEST_CASE("an invalid fragment does not run the timeout") {
    const TestFrame frame = makeTestFrame(THREE_FRAGMENTS, 5);
    fm::Reassembler reassembler;
    REQUIRE(feed(reassembler, frame, {0}, START) == Status::ACCEPTED);

    fm::VideoFragment bad = frame.fragments[1];
    bad.codec = 2;
    CHECK(reassembler.accept(bad, START + 200ms) == Status::INVALID);
    CHECK(reassembler.framesDropped() == 0);

    reassembler.expire(START + 200ms);
    CHECK(reassembler.framesDropped() == 1);
}

TEST_CASE("reset forgets frames, the last delivered frameId and the counters") {
    const TestFrame older = makeTestFrame(100, 5);
    const TestFrame delivered = makeTestFrame(100, 10);
    const TestFrame partial = makeTestFrame(2 * fm::MAX_FRAGMENT_PAYLOAD, 11);
    fm::Reassembler reassembler;
    REQUIRE(feedAll(reassembler, delivered, START) == Status::FRAME_COMPLETE);
    REQUIRE(feed(reassembler, partial, {0}, START) == Status::ACCEPTED);

    reassembler.reset();
    CHECK(reassembler.framesCompleted() == 0);
    CHECK(reassembler.framesDropped() == 0);
    CHECK(reassembler.completedFrame().data.empty());
    CHECK(feedAll(reassembler, older, START) == Status::FRAME_COMPLETE);
    CHECK(feed(reassembler, partial, {1}, START) == Status::ACCEPTED);
}

TEST_CASE("fragmenter to reassembler round trip at the size limits") {
    for (const std::size_t size : {std::size_t{1}, std::size_t{50'000}, std::size_t{fm::MAX_FRAME_SIZE}}) {
        CAPTURE(size);
        const TestFrame frame = makeTestFrame(size, 1);
        fm::Reassembler reassembler;
        Status status = Status::INVALID;
        for (const std::size_t index : shuffledOrder(frame)) {
            status = reassembler.accept(frame.fragments[index], START);
        }
        CHECK(status == Status::FRAME_COMPLETE);
        CHECK(holds(reassembler, frame));
    }
}
