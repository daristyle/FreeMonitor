#include "CursorSender.hpp"
#include "Packets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <semaphore>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
    using Clock = std::chrono::steady_clock;

    const Clock::time_point START = Clock::time_point{} + 1h;
    constexpr std::uint32_t SESSION_ID = 0xC0FF'EE01;
    constexpr std::uint32_t FIRST_SEQ = 500;
    constexpr std::chrono::milliseconds RECEIVE_TIMEOUT{1000};
    constexpr std::chrono::milliseconds NO_MORE_TIMEOUT{50};

    class FakeCursorSource final : public fm::CursorSource {
    private:
        std::mutex mutex;
        std::counting_semaphore<> signal{0};
        std::optional<fm::CursorState> pending;
        bool loseNext = false;

    public:
        fm::CursorImage currentImage;
        std::atomic<int> wakeCount{0};

        fm::Result<> start(const fm::CaptureTarget&) override {
            return {};
        }

        void stop() override {}

        fm::CursorStatus waitForChange(fm::CursorState& state, std::chrono::milliseconds timeout) noexcept override {
            static_cast<void>(signal.try_acquire_for(timeout));
            const std::lock_guard lock(mutex);
            if (loseNext) {
                return fm::CursorStatus::CURSOR_LOST;
            }
            if (pending) {
                state = *std::exchange(pending, std::nullopt);
                return fm::CursorStatus::CHANGED;
            }
            return fm::CursorStatus::UNCHANGED;
        }

        void wake() noexcept override {
            ++wakeCount;
            signal.release();
        }

        const fm::CursorImage& image() const noexcept override {
            return currentImage;
        }

        void push(const fm::CursorState& state) {
            {
                const std::lock_guard lock(mutex);
                pending = state;
            }
            signal.release();
        }

        void lose() {
            {
                const std::lock_guard lock(mutex);
                loseNext = true;
            }
            signal.release();
        }
    };

    fm::UDPSocket openLoopbackSocket() {
        auto udpSocket = fm::UDPSocket::open(fm::Endpoint{fm::LOOPBACK_IP_ADDRESS, 0});
        REQUIRE(udpSocket);
        return std::move(*udpSocket);
    }

    fm::Endpoint localEndpointOf(const fm::UDPSocket& udpSocket) {
        const auto endpoint = udpSocket.localEndpoint();
        REQUIRE(endpoint);
        return *endpoint;
    }

    fm::CursorImage makeImage(std::uint16_t side) {
        return fm::CursorImage{.width = side,
                               .height = side,
                               .hotspotX = 5,
                               .hotspotY = 1,
                               .pixels = std::vector<std::uint8_t>(std::size_t{side} * side * 4, 0x80)};
    }

    struct Fixture {
        fm::UDPSocket sender = openLoopbackSocket();
        fm::UDPSocket receiver = openLoopbackSocket();
        FakeCursorSource source;
        fm::SequenceCounter seq{FIRST_SEQ};
        fm::CursorSender cursorSender;

        explicit Fixture(std::optional<std::chrono::milliseconds> shapeRepeatInterval = std::nullopt)
            : cursorSender(sender, source, shapeRepeatInterval) {
            source.currentImage = makeImage(24);
        }

        fm::CursorTarget target() {
            return fm::CursorTarget{.peer = localEndpointOf(receiver), .sessionId = SESSION_ID, .seq = seq};
        }

        std::vector<std::vector<std::uint8_t>> receive(std::size_t count) {
            std::vector<std::vector<std::uint8_t>> packets;
            std::vector<std::uint8_t> buffer(fm::MAX_PACKET_SIZE);
            for (std::size_t index = 0; index < count; ++index) {
                fm::Endpoint from;
                const auto result = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
                REQUIRE(result.status == fm::ReceiveStatus::RECEIVED);
                packets.emplace_back(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(result.size));
            }
            return packets;
        }

        bool nothingMoreArrives() {
            std::vector<std::uint8_t> buffer(fm::MAX_PACKET_SIZE);
            fm::Endpoint from;
            return receiver.receiveFrom(buffer, from, NO_MORE_TIMEOUT).status == fm::ReceiveStatus::NO_DATAGRAM;
        }

        void sendFirst(const fm::CursorState& state) {
            cursorSender.update(state);
            cursorSender.setTarget(target());
            static_cast<void>(cursorSender.sendDue(START));
            receive(3);
        }
    };

    fm::PacketType typeOf(const std::vector<std::uint8_t>& packet) {
        return static_cast<fm::PacketType>(packet[3]);
    }

    fm::CursorPos parsePos(const std::vector<std::uint8_t>& packet) {
        fm::CursorPos parsed;
        REQUIRE(parsed.read(packet));
        REQUIRE(typeOf(packet) == fm::PacketType::CURSOR_POS);
        return parsed;
    }

    fm::CursorShape parseShape(const std::vector<std::uint8_t>& packet) {
        fm::CursorShape parsed;
        REQUIRE(parsed.read(packet));
        REQUIRE(typeOf(packet) == fm::PacketType::CURSOR_SHAPE);
        return parsed;
    }

    constexpr fm::CursorState FIRST_STATE{.x = 100, .y = 200, .visible = true, .imageId = 1};
}

TEST_CASE("nothing is sent without a target, or before the source has reported a state") {
    Fixture fixture;
    fixture.cursorSender.update(FIRST_STATE);
    CHECK(fixture.cursorSender.sendDue(START) == START + fm::CursorSender::POSITION_REPEAT_INTERVAL);
    CHECK(fixture.nothingMoreArrives());

    Fixture noState;
    noState.cursorSender.setTarget(noState.target());
    static_cast<void>(noState.cursorSender.sendDue(START));
    CHECK(noState.nothingMoreArrives());
}

TEST_CASE("a new target gets the shape first, then the position, numbered by the session's own counter") {
    Fixture fixture;
    fixture.cursorSender.update(FIRST_STATE);
    fixture.cursorSender.setTarget(fixture.target());
    CHECK(fixture.source.wakeCount == 1);
    CHECK(fixture.cursorSender.sendDue(START) == START + fm::CursorSender::POSITION_REPEAT_INTERVAL);

    const auto packets = fixture.receive(3);
    for (std::size_t index = 0; index < 2; ++index) {
        const auto shape = parseShape(packets[index]);
        CHECK(shape.commonHeader.sessionId == SESSION_ID);
        CHECK(shape.commonHeader.seq == FIRST_SEQ + index);
        CHECK(shape.shapeId == 1);
        CHECK(shape.fragIndex == index);
        CHECK(shape.fragCount == 2);
        CHECK(shape.hotspotX == 5);
    }
    const auto position = parsePos(packets[2]);
    CHECK(position.commonHeader.seq == FIRST_SEQ + 2);
    CHECK(position.shapeId == 1);
    CHECK(position.x == 100);
    CHECK(position.y == 200);
    CHECK(position.flags == fm::CursorFlags::VISIBLE);
    CHECK(fixture.seq.peek() == FIRST_SEQ + 3);
    CHECK(fixture.nothingMoreArrives());

    const auto counts = fixture.cursorSender.counts();
    CHECK(counts.packets == 3);
    CHECK(counts.bytes == 2 * fm::CursorShape::SIZE + 24 * 24 * 4 + fm::CursorPos::SIZE);
}

TEST_CASE("position changes go out at most every 4 ms, and the newest one wins") {
    Fixture fixture;
    fixture.sendFirst(FIRST_STATE);

    fixture.cursorSender.update({.x = 101, .y = 200, .visible = true, .imageId = 1});
    CHECK(fixture.cursorSender.sendDue(START + 1ms) == START + 4ms);
    CHECK(fixture.nothingMoreArrives());

    fixture.cursorSender.update({.x = 102, .y = 201, .visible = true, .imageId = 1});
    static_cast<void>(fixture.cursorSender.sendDue(START + 4ms));
    const auto position = parsePos(fixture.receive(1)[0]);
    CHECK(position.x == 102);
    CHECK(position.y == 201);
    CHECK(fixture.nothingMoreArrives());
}

TEST_CASE("the latest state is repeated every 250 ms even when nothing changes") {
    Fixture fixture;
    fixture.sendFirst(FIRST_STATE);

    CHECK(fixture.cursorSender.sendDue(START + 249ms) == START + 250ms);
    CHECK(fixture.nothingMoreArrives());

    CHECK(fixture.cursorSender.sendDue(START + 250ms) == START + 500ms);
    const auto position = parsePos(fixture.receive(1)[0]);
    CHECK(position.commonHeader.seq == FIRST_SEQ + 3);
    CHECK(position.x == 100);
}

TEST_CASE("a hidden cursor is sent as a position without VISIBLE") {
    Fixture fixture;
    fixture.sendFirst(FIRST_STATE);

    fixture.cursorSender.update({.x = 100, .y = 200, .visible = false, .imageId = 1});
    static_cast<void>(fixture.cursorSender.sendDue(START + 10ms));
    CHECK(parsePos(fixture.receive(1)[0]).flags == 0);
}

TEST_CASE("a new image is sent before the first position that uses it") {
    Fixture fixture;
    fixture.sendFirst(FIRST_STATE);

    fixture.source.currentImage = makeImage(32);
    fixture.cursorSender.update({.x = 100, .y = 200, .visible = true, .imageId = 2});
    static_cast<void>(fixture.cursorSender.sendDue(START + 10ms));
    const auto packets = fixture.receive(5);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto shape = parseShape(packets[index]);
        CHECK(shape.shapeId == 2);
        CHECK(shape.fragCount == 4);
    }
    CHECK(parsePos(packets[4]).shapeId == 2);
    CHECK(fixture.nothingMoreArrives());
}

TEST_CASE("a shape request resends the current shape, and a request for an older one is ignored") {
    Fixture fixture;
    fixture.sendFirst(FIRST_STATE);

    fixture.cursorSender.requestShape(1);
    static_cast<void>(fixture.cursorSender.sendDue(START + 10ms));
    const auto packets = fixture.receive(2);
    CHECK(parseShape(packets[0]).shapeId == 1);
    CHECK(parseShape(packets[1]).fragIndex == 1);
    CHECK(fixture.nothingMoreArrives());

    fixture.cursorSender.requestShape(7);
    static_cast<void>(fixture.cursorSender.sendDue(START + 20ms));
    CHECK(fixture.nothingMoreArrives());
}

TEST_CASE("clearing the target stops every cursor packet, and a new target starts again with the shape") {
    Fixture fixture;
    fixture.sendFirst(FIRST_STATE);

    fixture.cursorSender.setTarget(std::nullopt);
    fixture.cursorSender.update({.x = 300, .y = 200, .visible = true, .imageId = 1});
    static_cast<void>(fixture.cursorSender.sendDue(START + 300ms));
    CHECK(fixture.nothingMoreArrives());

    fm::SequenceCounter otherSeq(900);
    fixture.cursorSender.setTarget(
            fm::CursorTarget{.peer = localEndpointOf(fixture.receiver), .sessionId = 2, .seq = otherSeq});
    static_cast<void>(fixture.cursorSender.sendDue(START + 310ms));
    const auto packets = fixture.receive(3);
    CHECK(parseShape(packets[0]).commonHeader.sessionId == 2);
    CHECK(parseShape(packets[0]).commonHeader.seq == 900);
    CHECK(parsePos(packets[2]).x == 300);
    CHECK(otherSeq.peek() == 903);
}

TEST_CASE("development mode resends the shape on a timer, since its receiver cannot ask for it") {
    Fixture fixture(1000ms);
    fixture.sendFirst(FIRST_STATE);

    CHECK(fixture.cursorSender.sendDue(START + 999ms) == START + 1000ms);
    CHECK(parsePos(fixture.receive(1)[0]).x == 100);
    CHECK(fixture.nothingMoreArrives());

    static_cast<void>(fixture.cursorSender.sendDue(START + 1000ms));
    const auto packets = fixture.receive(2);
    CHECK(parseShape(packets[0]).shapeId == 1);
    CHECK(parseShape(packets[1]).shapeId == 1);
    CHECK(fixture.nothingMoreArrives());
}

TEST_CASE("the thread sends what the source reports and stops promptly") {
    Fixture fixture;
    fixture.cursorSender.start();
    fixture.cursorSender.setTarget(fixture.target());
    fixture.source.push(FIRST_STATE);

    const auto packets = fixture.receive(3);
    CHECK(typeOf(packets[0]) == fm::PacketType::CURSOR_SHAPE);
    CHECK(parsePos(packets[2]).x == 100);

    fixture.source.push({.x = 150, .y = 200, .visible = true, .imageId = 1});
    CHECK(parsePos(fixture.receive(1)[0]).x == 150);

    const auto stopStart = Clock::now();
    fixture.cursorSender.stop();
    CHECK(Clock::now() - stopStart < 200ms);
    CHECK_FALSE(fixture.cursorSender.lost());
}

TEST_CASE("the thread ends and reports it when the cursor source is lost") {
    Fixture fixture;
    fixture.cursorSender.start();
    fixture.source.lose();
    const auto deadline = Clock::now() + 1s;
    while (!fixture.cursorSender.lost() && Clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(fixture.cursorSender.lost());
    fixture.cursorSender.stop();
}
