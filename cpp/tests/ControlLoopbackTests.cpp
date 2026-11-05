#include "ControlTestHelpers.hpp"
#include "HostSessions.hpp"
#include "TabletSession.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace {
    constexpr std::chrono::milliseconds STEP{5};
    constexpr std::chrono::milliseconds LATENCY{5};
    constexpr std::uint32_t PERCENT = 100;
    constexpr fm::HostOffer OFFER{.width = 2944, .height = 1840, .fps = 60, .codecs = BOTH_CODECS, .features = 0};
    constexpr fm::TabletSettings SETTINGS{
            .host = HOST_ADDRESS, .width = 2944, .height = 1840, .refreshHz = 60, .codecs = BOTH_CODECS, .features = 0};

    struct Delivery {
        std::vector<std::uint8_t> bytes;
        fm::Endpoint from;
        fm::Endpoint to;
        std::chrono::steady_clock::time_point arrival;
    };

    class SimulatedNetwork {
    private:
        std::mt19937 random;
        std::vector<Delivery> inFlight;

    public:
        std::uint32_t lossPercent = 0;
        std::chrono::steady_clock::time_point now = START;

        explicit SimulatedNetwork(std::uint32_t seed) : random(seed) {}

        void carry(std::span<const std::uint8_t> packet, const fm::Endpoint& from, const fm::Endpoint& to) {
            if (random() % PERCENT < lossPercent) {
                return;
            }
            inFlight.push_back(Delivery{{packet.begin(), packet.end()}, from, to, now + LATENCY});
        }

        std::vector<Delivery> arrived() {
            std::vector<Delivery> due;
            std::vector<Delivery> later;
            for (Delivery& delivery : inFlight) {
                (delivery.arrival <= now ? due : later).push_back(std::move(delivery));
            }
            inFlight = std::move(later);
            return due;
        }
    };

    class NetworkSide final : public fm::ControlTransport {
    private:
        SimulatedNetwork& network;
        fm::Endpoint self;

    public:
        NetworkSide(SimulatedNetwork& network, fm::Endpoint self) : network(network), self(self) {}

        void send(std::span<const std::uint8_t> packet, const fm::Endpoint& to) noexcept override {
            network.carry(packet, self, to);
        }
    };

    struct Simulation {
        SimulatedNetwork network;
        NetworkSide hostSide;
        NetworkSide tabletSide;
        RecordingListener listener;
        fm::HostSessions host;
        fm::TabletSession tablet;
        bool keyframeRequested = false;

        explicit Simulation(std::uint32_t seed)
            : network(seed), hostSide(network, HOST_ADDRESS), tabletSide(network, TABLET_ADDRESS),
              host(hostSide, listener, OFFER, counterFrom(70000)), tablet(tabletSide, SETTINGS, counterFrom(300)) {}

        void run(std::chrono::milliseconds duration) {
            const auto end = network.now + duration;
            while (network.now < end) {
                network.now += STEP;
                for (const Delivery& delivery : network.arrived()) {
                    if (delivery.to == HOST_ADDRESS) {
                        host.handlePacket(delivery.bytes, delivery.from, network.now);
                    } else {
                        tablet.handlePacket(delivery.bytes, delivery.from, network.now);
                    }
                }
                host.poll(network.now);
                tablet.poll(network.now);
                if (host.takeKeyframeRequest()) {
                    keyframeRequested = true;
                    host.keyframeSent(network.now);
                    tablet.keyframeReceived();
                }
            }
        }

        bool connected() {
            const fm::Session* session = host.streamingSession();
            return tablet.state() == fm::TabletState::CONNECTED && session != nullptr &&
                   session->sessionId == tablet.sessionId();
        }
    };
}

TEST_CASE("a session comes up and stays up with 20% loss in both directions") {
    Simulation simulation(GENERATE(1U, 2U, 3U, 4U, 5U));
    simulation.network.lossPercent = 20;
    simulation.tablet.start(simulation.network.now);
    simulation.run(10s);

    CHECK(simulation.connected());
    CHECK(simulation.tablet.sessionCount() == 1);
    CHECK(simulation.listener.ended.empty());
    const auto report = simulation.host.report();
    REQUIRE(report);
    CHECK(report->clockSample);
    CHECK(report->stats);
    CHECK(simulation.tablet.roundTripTime());
}

TEST_CASE("after a 5 s blackout both sides time out, and the tablet connects again with a new session") {
    Simulation simulation(GENERATE(1U, 2U, 3U));
    simulation.tablet.start(simulation.network.now);
    simulation.run(2s);
    REQUIRE(simulation.connected());
    const std::uint32_t firstSession = simulation.tablet.sessionId();

    simulation.network.lossPercent = PERCENT;
    simulation.run(5s);
    CHECK(simulation.host.streamingSession() == nullptr);
    CHECK(simulation.listener.ended == std::vector{std::pair{firstSession, fm::SessionEndReason::TIMEOUT}});

    simulation.network.lossPercent = 0;
    simulation.run(3s);
    CHECK(simulation.connected());
    CHECK(simulation.tablet.sessionId() != firstSession);
}

TEST_CASE("a keyframe request gets through 50% loss, because the tablet repeats it") {
    Simulation simulation(GENERATE(1U, 2U, 3U));
    simulation.tablet.start(simulation.network.now);
    simulation.run(2s);
    REQUIRE(simulation.connected());
    simulation.keyframeRequested = false;

    simulation.network.lossPercent = 50;
    simulation.tablet.requestKeyframe(simulation.network.now);
    simulation.run(2s);
    CHECK(simulation.keyframeRequested);
}
