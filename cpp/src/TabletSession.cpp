#include "TabletSession.hpp"

#include "Timestamp.hpp"

#include <limits>
#include <utility>

namespace fm {
    namespace {
        constexpr std::chrono::milliseconds HELLO_RESEND_INTERVAL{HELLO_RESEND_INTERVAL_MS};
        constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL{HEARTBEAT_INTERVAL_MS};
        constexpr std::chrono::milliseconds SESSION_TIMEOUT{SESSION_TIMEOUT_MS};
        constexpr std::chrono::milliseconds BYE_REPEAT_INTERVAL{BYE_REPEAT_INTERVAL_MS};
        constexpr std::chrono::milliseconds KEYFRAME_REQUEST_REPEAT_INTERVAL{KEYFRAME_REQUEST_REPEAT_INTERVAL_MS};
        constexpr std::chrono::milliseconds STATS_INTERVAL{STATS_INTERVAL_MS};
        constexpr std::uint8_t KNOWN_FEATURES = FeatureFlags::CURSOR_OVERLAY | FeatureFlags::FEC;
    }

    TabletSession::TabletSession(ControlTransport& transport, TabletSettings settings,
                                 std::function<std::uint32_t()> randomNumber)
        : transport(transport), settings(settings), randomNumber(std::move(randomNumber)) {}

    void TabletSession::start(std::chrono::steady_clock::time_point now) noexcept {
        std::uint32_t newSessionId = 0;
        while (newSessionId == 0 || newSessionId == session.sessionId) {
            newSessionId = randomNumber();
        }
        session = Session{.peer = settings.host, .sessionId = newSessionId, .seq = SequenceCounter(randomNumber())};
        host.reset(settings.host, now);
        reliableSender.reset();
        reliableReceiver.reset();
        heartbeatEcho.reset();
        helloAcknowledged = false;
        currentStream = {};
        keyframePending = false;
        framesCompleted = 0;
        framesDropped = 0;
        currentState = TabletState::CONNECTING;
        ++sessionsStarted;

        Hello hello;
        hello.width = settings.width;
        hello.height = settings.height;
        hello.refreshHz = settings.refreshHz;
        hello.codecs = settings.codecs;
        hello.features = settings.features;
        const std::size_t size = writeControlPacket(hello, session.sessionId, 0, 0, sendBuffer);
        helloMsgId = reliableSender.enqueue(std::span(sendBuffer).first(size), now);
        poll(now);
    }

    SessionPacket TabletSession::handlePacket(std::span<const std::uint8_t> packet, const Endpoint& from,
                                              std::chrono::steady_clock::time_point now) noexcept {
        CommonHeader header;
        if (!sessionExists() || !header.read(packet) || header.sessionId != session.sessionId) {
            return SessionPacket::IGNORED;
        }
        if (checkControlPacket(packet) == ControlPacketCheck::VALID) {
            const auto type = static_cast<PacketType>(header.type);
            if (type == PacketType::WELCOME) {
                Welcome welcome;
                welcome.read(packet);
                if (!acceptWelcome(welcome)) {
                    return SessionPacket::IGNORED;
                }
            }
            host.notePacket(from, header.seq, packet.size(), now);
            session.peer = host.address();
            handleControl(type, packet, now);
            return SessionPacket::CONTROL;
        }
        if (currentState == TabletState::CONNECTED && isMediaPacket(header)) {
            host.notePacket(from, header.seq, packet.size(), now);
            session.peer = host.address();
            return SessionPacket::MEDIA;
        }
        return SessionPacket::IGNORED;
    }

    void TabletSession::handleControl(PacketType type, std::span<const std::uint8_t> packet,
                                      std::chrono::steady_clock::time_point now) noexcept {
        switch (type) {
            case PacketType::ACK: {
                ACK ack;
                ack.read(packet);
                if (reliableSender.acknowledge(ack.ackedMsgId, now) && helloMsgId == ack.ackedMsgId) {
                    helloAcknowledged = true;
                }
                break;
            }
            case PacketType::WELCOME: {
                Welcome welcome;
                welcome.read(packet);
                sendAck(welcome.controlHeader.msgId, now);
                if (!reliableReceiver.shouldProcess(welcome.controlHeader.msgId) ||
                    currentState != TabletState::CONNECTING) {
                    break;
                }
                if (helloMsgId && reliableSender.acknowledge(*helloMsgId, now)) {
                    helloAcknowledged = true;
                }
                currentStream = StreamInfo{.width = welcome.width,
                                           .height = welcome.height,
                                           .fps = welcome.fps,
                                           .codec = static_cast<Codec>(welcome.codec),
                                           .features = static_cast<std::uint8_t>(welcome.features & KNOWN_FEATURES)};
                currentState = TabletState::CONNECTED;
                nextHeartbeatTime = now;
                nextStatsTime = now + STATS_INTERVAL;
                break;
            }
            case PacketType::HEARTBEAT: {
                Heartbeat heartbeat;
                heartbeat.read(packet);
                heartbeatEcho.received(heartbeat, now);
                break;
            }
            case PacketType::BYE:
                currentState = TabletState::ENDED_BY_HOST;
                reliableSender.reset();
                keyframePending = false;
                break;
            case PacketType::REJECT: {
                Reject reject;
                reject.read(packet);
                lastRejectReason = reject.reason;
                currentState = TabletState::REJECTED;
                reliableSender.reset();
                break;
            }
            default:
                break;
        }
    }

    void TabletSession::poll(std::chrono::steady_clock::time_point now) noexcept {
        sendDueBye(now);
        if (currentState != TabletState::CONNECTING && currentState != TabletState::CONNECTED) {
            return;
        }
        if ((currentState == TabletState::CONNECTED || helloAcknowledged) && host.silentFor(now, SESSION_TIMEOUT)) {
            start(now);
            return;
        }
        if (const auto due = reliableSender.due(now, HELLO_RESEND_INTERVAL); !due.empty()) {
            restampHeader(due, session.seq.next(), toTimestampUs(now));
            transport.send(due, session.peer);
        }
        if (currentState != TabletState::CONNECTED) {
            return;
        }
        if (now >= nextHeartbeatTime) {
            sendPacket(session, writeControlPacket(heartbeatEcho.nextHeartbeat(now), session.sessionId,
                                                   session.seq.next(), toTimestampUs(now), sendBuffer));
            nextHeartbeatTime = now + HEARTBEAT_INTERVAL;
        }
        if (now >= nextStatsTime) {
            sendStats(now);
            nextStatsTime = now + STATS_INTERVAL;
        }
        if (keyframePending && now >= nextKeyframeRequestTime) {
            sendKeyframeRequest(now);
        }
    }

    void TabletSession::close(std::chrono::steady_clock::time_point now) noexcept {
        if (!sessionExists()) {
            return;
        }
        byeSession = session;
        byesRemaining = BYE_REPEAT_COUNT;
        nextByeTime = now;
        currentState = TabletState::IDLE;
        reliableSender.reset();
        keyframePending = false;
        sendDueBye(now);
    }

    bool TabletSession::closing() const noexcept {
        return byesRemaining > 0;
    }

    void TabletSession::requestKeyframe(std::chrono::steady_clock::time_point now) noexcept {
        if (currentState != TabletState::CONNECTED || keyframePending) {
            return;
        }
        keyframePending = true;
        sendKeyframeRequest(now);
    }

    void TabletSession::sendShapeRequest(std::uint32_t shapeId, std::chrono::steady_clock::time_point now) noexcept {
        if (currentState != TabletState::CONNECTED) {
            return;
        }
        ShapeRequest request;
        request.shapeId = shapeId;
        sendPacket(session,
                   writeControlPacket(request, session.sessionId, session.seq.next(), toTimestampUs(now), sendBuffer));
    }

    void TabletSession::keyframeReceived() noexcept {
        keyframePending = false;
    }

    void TabletSession::frameCompleted() noexcept {
        ++framesCompleted;
    }

    void TabletSession::frameDropped() noexcept {
        ++framesDropped;
    }

    TabletState TabletSession::state() const noexcept {
        return currentState;
    }

    std::uint32_t TabletSession::sessionId() const noexcept {
        return session.sessionId;
    }

    const StreamInfo& TabletSession::stream() const noexcept {
        return currentStream;
    }

    std::uint8_t TabletSession::rejectReason() const noexcept {
        return lastRejectReason;
    }

    std::optional<std::chrono::microseconds> TabletSession::roundTripTime() const noexcept {
        return heartbeatEcho.roundTripTime();
    }

    std::uint32_t TabletSession::sessionCount() const noexcept {
        return sessionsStarted;
    }

    bool TabletSession::sessionExists() const noexcept {
        return currentState == TabletState::CONNECTING || currentState == TabletState::CONNECTED;
    }

    bool TabletSession::acceptWelcome(const Welcome& welcome) const noexcept {
        if (welcome.codec >= std::numeric_limits<std::uint8_t>::digits) {
            return false;
        }
        const auto codecFlag = static_cast<std::uint8_t>(1U << welcome.codec);
        return (settings.codecs & codecFlag) != 0 && welcome.width != 0 && welcome.height != 0 &&
               welcome.width % 2 == 0 && welcome.height % 2 == 0 &&
               (welcome.features & KNOWN_FEATURES & ~settings.features) == 0;
    }

    void TabletSession::sendPacket(const Session& target, std::size_t size) noexcept {
        transport.send(std::span(sendBuffer).first(size), target.peer);
    }

    void TabletSession::sendAck(std::uint32_t msgId, std::chrono::steady_clock::time_point now) noexcept {
        ACK ack;
        ack.ackedMsgId = msgId;
        sendPacket(session,
                   writeControlPacket(ack, session.sessionId, session.seq.next(), toTimestampUs(now), sendBuffer));
    }

    void TabletSession::sendKeyframeRequest(std::chrono::steady_clock::time_point now) noexcept {
        sendPacket(session, writeControlPacket(KeyframeRequest{}, session.sessionId, session.seq.next(),
                                               toTimestampUs(now), sendBuffer));
        nextKeyframeRequestTime = now + KEYFRAME_REQUEST_REPEAT_INTERVAL;
    }

    void TabletSession::sendStats(std::chrono::steady_clock::time_point now) noexcept {
        Stats stats;
        stats.packetsReceived = host.received();
        stats.packetsLost = host.lost();
        stats.framesCompleted = framesCompleted;
        stats.framesDropped = framesDropped;
        stats.bytesReceived = host.bytes();
        sendPacket(session,
                   writeControlPacket(stats, session.sessionId, session.seq.next(), toTimestampUs(now), sendBuffer));
    }

    void TabletSession::sendDueBye(std::chrono::steady_clock::time_point now) noexcept {
        if (byesRemaining == 0 || now < nextByeTime) {
            return;
        }
        sendPacket(byeSession, writeControlPacket(Bye{}, byeSession.sessionId, byeSession.seq.next(),
                                                  toTimestampUs(now), sendBuffer));
        --byesRemaining;
        nextByeTime = now + BYE_REPEAT_INTERVAL;
    }
}
