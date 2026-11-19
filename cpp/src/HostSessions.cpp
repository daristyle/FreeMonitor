#include "HostSessions.hpp"

#include "SerialNumber.hpp"
#include "Timestamp.hpp"

#include <utility>

namespace fm {
    namespace {
        constexpr std::chrono::milliseconds RELIABLE_RESEND_INTERVAL{RELIABLE_RESEND_INTERVAL_MS};
        constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL{HEARTBEAT_INTERVAL_MS};
        constexpr std::chrono::milliseconds SESSION_TIMEOUT{SESSION_TIMEOUT_MS};
        constexpr std::chrono::milliseconds BYE_REPEAT_INTERVAL{BYE_REPEAT_INTERVAL_MS};
        constexpr std::chrono::milliseconds KEYFRAME_REQUEST_IGNORE{KEYFRAME_REQUEST_IGNORE_MS};
    }

    HostSessions::HostSessions(ControlTransport& transport, HostSessionListener& listener, HostOffer offer,
                               std::function<std::uint32_t()> randomNumber, std::size_t maxSessions)
        : transport(transport), listener(listener), offer(offer), randomNumber(std::move(randomNumber)),
          maxSessions(maxSessions) {
        clients.reserve(maxSessions);
    }

    void HostSessions::setOffer(const HostOffer& newOffer) noexcept {
        offer = newOffer;
    }

    void HostSessions::handlePacket(std::span<const std::uint8_t> packet, const Endpoint& from,
                                    std::chrono::steady_clock::time_point now) noexcept {
        const ControlPacketCheck check = checkControlPacket(packet);
        CommonHeader header;
        if (!header.read(packet)) {
            return;
        }
        if (check == ControlPacketCheck::UNSUPPORTED_VERSION) {
            reject(header.sessionId, RejectReason::UNSUPPORTED_VERSION, from, now);
            return;
        }
        if (check != ControlPacketCheck::VALID) {
            return;
        }
        const auto type = static_cast<PacketType>(header.type);
        if (type == PacketType::HELLO) {
            handleHello(packet, from, now);
            return;
        }
        for (std::size_t index = 0; index < clients.size(); ++index) {
            if (clients[index].session.sessionId == header.sessionId) {
                handleSessionPacket(index, type, header, packet, from, now);
                return;
            }
        }
    }

    void HostSessions::handleHello(std::span<const std::uint8_t> packet, const Endpoint& from,
                                   std::chrono::steady_clock::time_point now) noexcept {
        Hello hello;
        hello.read(packet);
        const std::uint32_t sessionId = hello.commonHeader.sessionId;
        if (Client* existing = findClient(sessionId)) {
            existing->peer.notePacket(from, hello.commonHeader.seq, packet.size(), now);
            existing->session.peer = existing->peer.address();
            existing->reliableReceiver.shouldProcess(hello.controlHeader.msgId);
            sendAck(*existing, hello.controlHeader.msgId, now);
            return;
        }
        if (hello.width == 0 || hello.height == 0 || (hello.codecs & CodecFlags::JPEG) == 0) {
            reject(sessionId, RejectReason::INVALID_PARAMETERS, from, now);
            return;
        }
        const auto commonCodecs = static_cast<std::uint8_t>(hello.codecs & offer.codecs);
        if (commonCodecs == 0) {
            reject(sessionId, RejectReason::NO_COMMON_CODEC, from, now);
            return;
        }
        if ((hello.features & offer.requiredFeatures) != offer.requiredFeatures) {
            reject(sessionId, RejectReason::MISSING_FEATURE, from, now);
            return;
        }
        while (!clients.empty() && clients.size() >= maxSessions) {
            endSession(0, SessionEndReason::REPLACED, true, now);
        }

        Client& client = clients.emplace_back();
        client.session = Session{.peer = from, .sessionId = sessionId, .seq = SequenceCounter(randomNumber())};
        client.peer.reset(from, now);
        client.peer.notePacket(from, hello.commonHeader.seq, packet.size(), now);
        client.reliableReceiver.shouldProcess(hello.controlHeader.msgId);
        client.tablet = TabletInfo{.width = hello.width,
                                   .height = hello.height,
                                   .refreshHz = hello.refreshHz,
                                   .codecs = hello.codecs,
                                   .features = hello.features};
        client.stream = StreamInfo{.width = offer.width,
                                   .height = offer.height,
                                   .fps = offer.fps,
                                   .codec = (commonCodecs & CodecFlags::H264) != 0 ? Codec::H264 : Codec::JPEG,
                                   .features = static_cast<std::uint8_t>(offer.features & hello.features)};
        client.session.features = client.stream.features;
        client.nextHeartbeatTime = now + HEARTBEAT_INTERVAL;

        sendAck(client, hello.controlHeader.msgId, now);
        Welcome welcome;
        welcome.width = client.stream.width;
        welcome.height = client.stream.height;
        welcome.fps = client.stream.fps;
        welcome.codec = static_cast<std::uint8_t>(client.stream.codec);
        welcome.features = client.stream.features;
        const std::size_t size = writeControlPacket(welcome, sessionId, 0, 0, sendBuffer);
        client.welcomeMsgId = client.reliableSender.enqueue(std::span(sendBuffer).first(size), now);
        listener.sessionStarted(sessionId, client.tablet, client.stream);
        poll(now);
    }

    void HostSessions::handleSessionPacket(std::size_t clientIndex, PacketType type, const CommonHeader& header,
                                           std::span<const std::uint8_t> packet, const Endpoint& from,
                                           std::chrono::steady_clock::time_point now) noexcept {
        Client& client = clients[clientIndex];
        client.peer.notePacket(from, header.seq, packet.size(), now);
        client.session.peer = client.peer.address();
        switch (type) {
            case PacketType::ACK: {
                ACK ack;
                ack.read(packet);
                client.reliableSender.acknowledge(ack.ackedMsgId, now);
                if (!client.streaming && client.welcomeMsgId == ack.ackedMsgId) {
                    client.streaming = true;
                    client.keyframeRequested = true;
                    listener.streamingStarted(client.session.sessionId);
                }
                break;
            }
            case PacketType::HEARTBEAT: {
                Heartbeat heartbeat;
                heartbeat.read(packet);
                client.heartbeatEcho.received(heartbeat, now);
                break;
            }
            case PacketType::KEYFRAME_REQUEST:
                if (client.streaming &&
                    !(client.lastKeyframeTime && now - *client.lastKeyframeTime < KEYFRAME_REQUEST_IGNORE)) {
                    client.keyframeRequested = true;
                }
                break;
            case PacketType::SHAPE_REQUEST: {
                ShapeRequest request;
                request.read(packet);
                if (client.streaming && (client.stream.features & FeatureFlags::CURSOR_OVERLAY) != 0) {
                    client.requestedShape = request.shapeId;
                }
                break;
            }
            case PacketType::STATS: {
                Stats stats;
                stats.read(packet);
                client.lastStats = stats;
                break;
            }
            case PacketType::BYE:
                endSession(clientIndex, SessionEndReason::BYE, false, now);
                break;
            default:
                break;
        }
    }

    void HostSessions::poll(std::chrono::steady_clock::time_point now) noexcept {
        for (std::size_t index = 0; index < clients.size();) {
            Client& client = clients[index];
            if (client.peer.silentFor(now, SESSION_TIMEOUT)) {
                endSession(index, SessionEndReason::TIMEOUT, false, now);
                continue;
            }
            if (const auto due = client.reliableSender.due(now, RELIABLE_RESEND_INTERVAL); !due.empty()) {
                restampHeader(due, client.session.seq.next(), toTimestampUs(now));
                transport.send(due, client.session.peer);
            }
            if (now >= client.nextHeartbeatTime) {
                sendHeartbeat(client, now);
                client.nextHeartbeatTime = now + HEARTBEAT_INTERVAL;
            }
            ++index;
        }
        sendDueByes(now);
    }

    Session* HostSessions::streamingSession() noexcept {
        Client* client = streamingClient();
        return client != nullptr ? &client->session : nullptr;
    }

    bool HostSessions::takeKeyframeRequest() noexcept {
        Client* client = streamingClient();
        return client != nullptr && std::exchange(client->keyframeRequested, false);
    }

    std::optional<std::uint32_t> HostSessions::takeShapeRequest() noexcept {
        Client* client = streamingClient();
        if (client == nullptr || client->requestedShape == 0) {
            return std::nullopt;
        }
        return std::exchange(client->requestedShape, 0);
    }

    void HostSessions::keyframeSent(std::chrono::steady_clock::time_point now) noexcept {
        if (Client* client = streamingClient()) {
            client->lastKeyframeTime = now;
        }
    }

    std::optional<TabletReport> HostSessions::report() const noexcept {
        const Client* client = streamingClient();
        if (client == nullptr) {
            return std::nullopt;
        }
        return TabletReport{.stats = client->lastStats, .clockSample = client->heartbeatEcho.clockSample()};
    }

    void HostSessions::close(std::chrono::steady_clock::time_point now) noexcept {
        while (!clients.empty()) {
            endSession(clients.size() - 1, SessionEndReason::CLOSED, true, now);
        }
    }

    bool HostSessions::closing() const noexcept {
        for (const PendingBye& bye : pendingByes) {
            if (bye.remaining > 0) {
                return true;
            }
        }
        return false;
    }

    HostSessions::Client* HostSessions::findClient(std::uint32_t sessionId) noexcept {
        for (Client& client : clients) {
            if (client.session.sessionId == sessionId) {
                return &client;
            }
        }
        return nullptr;
    }

    HostSessions::Client* HostSessions::streamingClient() noexcept {
        for (Client& client : clients) {
            if (client.streaming) {
                return &client;
            }
        }
        return nullptr;
    }

    const HostSessions::Client* HostSessions::streamingClient() const noexcept {
        for (const Client& client : clients) {
            if (client.streaming) {
                return &client;
            }
        }
        return nullptr;
    }

    void HostSessions::reject(std::uint32_t sessionId, RejectReason reason, const Endpoint& to,
                              std::chrono::steady_clock::time_point now) noexcept {
        Reject packet;
        packet.reason = static_cast<std::uint8_t>(reason);
        packet.version = PROTOCOL_VERSION;
        const std::size_t size = writeControlPacket(packet, sessionId, 0, toTimestampUs(now), sendBuffer);
        transport.send(std::span(sendBuffer).first(size), to);
        listener.helloRejected(sessionId, reason);
    }

    void HostSessions::endSession(std::size_t clientIndex, SessionEndReason reason, bool sendBye,
                                  std::chrono::steady_clock::time_point now) noexcept {
        const Session session = clients[clientIndex].session;
        clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(clientIndex));
        if (sendBye) {
            for (PendingBye& bye : pendingByes) {
                if (bye.remaining == 0) {
                    bye = PendingBye{.session = session, .remaining = BYE_REPEAT_COUNT, .nextSendTime = now};
                    break;
                }
            }
            sendDueByes(now);
        }
        listener.sessionEnded(session.sessionId, reason);
    }

    void HostSessions::sendPacket(const Session& session, std::size_t size) noexcept {
        transport.send(std::span(sendBuffer).first(size), session.peer);
    }

    void HostSessions::sendAck(Client& client, std::uint32_t msgId,
                               std::chrono::steady_clock::time_point now) noexcept {
        ACK packet;
        packet.ackedMsgId = msgId;
        sendPacket(client.session, writeControlPacket(packet, client.session.sessionId, client.session.seq.next(),
                                                      toTimestampUs(now), sendBuffer));
    }

    void HostSessions::sendHeartbeat(Client& client, std::chrono::steady_clock::time_point now) noexcept {
        sendPacket(client.session, writeControlPacket(client.heartbeatEcho.nextHeartbeat(now), client.session.sessionId,
                                                      client.session.seq.next(), toTimestampUs(now), sendBuffer));
    }

    void HostSessions::sendDueByes(std::chrono::steady_clock::time_point now) noexcept {
        for (PendingBye& bye : pendingByes) {
            if (bye.remaining == 0 || now < bye.nextSendTime) {
                continue;
            }
            sendPacket(bye.session, writeControlPacket(Bye{}, bye.session.sessionId, bye.session.seq.next(),
                                                       toTimestampUs(now), sendBuffer));
            --bye.remaining;
            bye.nextSendTime = now + BYE_REPEAT_INTERVAL;
        }
    }

    std::optional<StatsDifference> statsDifference(const Stats& earlier, const Stats& later) noexcept {
        if (earlier.commonHeader.sessionId != later.commonHeader.sessionId ||
            later.commonHeader.timestampUs <= earlier.commonHeader.timestampUs) {
            return std::nullopt;
        }
        StatsDifference difference;
        difference.interval = std::chrono::microseconds(
                static_cast<std::int64_t>(later.commonHeader.timestampUs - earlier.commonHeader.timestampUs));
        difference.packetsReceived = later.packetsReceived - earlier.packetsReceived;
        difference.packetsLost = serialDifference(later.packetsLost, earlier.packetsLost);
        difference.framesCompleted = later.framesCompleted - earlier.framesCompleted;
        difference.framesDropped = later.framesDropped - earlier.framesDropped;
        difference.bytesReceived = later.bytesReceived - earlier.bytesReceived;
        return difference;
    }
}
