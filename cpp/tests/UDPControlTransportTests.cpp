#include "ControlTestHelpers.hpp"
#include "HostSessions.hpp"
#include "TabletSession.hpp"
#include "UDPControlTransport.hpp"
#include "UDPSocket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::chrono::milliseconds RECEIVE_TIMEOUT{1000};
    constexpr std::chrono::milliseconds PUMP_TIMEOUT{2000};
    constexpr std::chrono::milliseconds PUMP_STEP{1};
    const fm::Endpoint ANY_LOOPBACK_PORT{fm::LOOPBACK_IP_ADDRESS, 0};
    constexpr fm::HostOffer OFFER{.width = 2944, .height = 1840, .fps = 60, .codecs = BOTH_CODECS, .features = 0};

    fm::UDPSocket openLoopbackSocket() {
        auto udpSocket = fm::UDPSocket::open(ANY_LOOPBACK_PORT);
        REQUIRE(udpSocket);
        return std::move(*udpSocket);
    }

    fm::Endpoint localEndpointOf(const fm::UDPSocket& udpSocket) {
        const auto endpoint = udpSocket.localEndpoint();
        REQUIRE(endpoint);
        return *endpoint;
    }

    template<typename Side>
    void deliver(fm::UDPSocket& udpSocket, Side& side, std::chrono::milliseconds timeout) {
        std::array<std::uint8_t, fm::MAX_CONTROL_PACKET_SIZE> buffer{};
        fm::Endpoint from;
        auto received = udpSocket.receiveFrom(buffer, from, timeout);
        while (received.status == fm::ReceiveStatus::RECEIVED) {
            side.handlePacket(std::span(buffer).first(received.size), from, received.arrivalTime);
            received = udpSocket.receiveFrom(buffer, from, std::chrono::milliseconds(0));
        }
    }

    struct LoopbackPair {
        fm::UDPSocket hostSocket = openLoopbackSocket();
        fm::UDPSocket tabletSocket = openLoopbackSocket();
        fm::UDPControlTransport hostTransport{hostSocket};
        fm::UDPControlTransport tabletTransport{tabletSocket};
        RecordingListener listener;
        fm::HostSessions host{hostTransport, listener, OFFER, counterFrom(70000)};
        fm::TabletSession tablet{tabletTransport,
                                 fm::TabletSettings{.host = localEndpointOf(hostSocket),
                                                    .width = 2944,
                                                    .height = 1840,
                                                    .refreshHz = 60,
                                                    .codecs = BOTH_CODECS,
                                                    .features = 0},
                                 counterFrom(300)};

        bool pumpUntil(const std::function<bool()>& condition) {
            const auto deadline = Clock::now() + PUMP_TIMEOUT;
            while (!condition()) {
                if (Clock::now() >= deadline) {
                    return false;
                }
                deliver(hostSocket, host, PUMP_STEP);
                deliver(tabletSocket, tablet, PUMP_STEP);
                host.poll(Clock::now());
                tablet.poll(Clock::now());
            }
            return true;
        }
    };
}

TEST_CASE("the UDP control transport sends each packet as one datagram to the given address") {
    auto sender = openLoopbackSocket();
    auto receiver = openLoopbackSocket();
    fm::UDPControlTransport transport(sender);
    const auto packet = byePacket(7, 42);
    transport.send(packet, localEndpointOf(receiver));

    std::array<std::uint8_t, fm::MAX_CONTROL_PACKET_SIZE + 1> buffer{};
    fm::Endpoint from;
    const auto received = receiver.receiveFrom(buffer, from, RECEIVE_TIMEOUT);
    REQUIRE(received.status == fm::ReceiveStatus::RECEIVED);
    CHECK(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + received.size) == packet);
    CHECK(from == localEndpointOf(sender));
}

TEST_CASE("a host and a tablet connect over real UDP sockets, pass a keyframe request and end with BYE") {
    LoopbackPair pair;
    pair.tablet.start(Clock::now());
    REQUIRE(pair.pumpUntil([&] {
        const fm::Session* session = pair.host.streamingSession();
        return pair.tablet.state() == fm::TabletState::CONNECTED && session != nullptr &&
               session->sessionId == pair.tablet.sessionId();
    }));
    CHECK(pair.host.streamingSession()->peer == localEndpointOf(pair.tabletSocket));
    REQUIRE(pair.host.takeKeyframeRequest());
    pair.host.keyframeSent(Clock::now());

    pair.tablet.requestKeyframe(Clock::now());
    REQUIRE(pair.pumpUntil([&] { return pair.host.takeKeyframeRequest(); }));

    const std::uint32_t sessionId = pair.tablet.sessionId();
    pair.tablet.close(Clock::now());
    REQUIRE(pair.pumpUntil([&] { return !pair.listener.ended.empty(); }));
    CHECK(pair.listener.ended == std::vector{std::pair{sessionId, fm::SessionEndReason::BYE}});
    CHECK(pair.host.streamingSession() == nullptr);
}
