#include "FrameReceiver.hpp"

#include <algorithm>
#include <utility>

namespace fm {
    namespace {
        bool isValidHeader(const CommonHeader& header) noexcept {
            return header.magic == MAGIC && header.version == PROTOCOL_VERSION && header.headerReserved == 0;
        }
    }

    FrameReceiver::FrameReceiver(UDPSocket udpSocket, std::uint32_t sessionId,
                                 std::chrono::milliseconds senderRestartTimeout,
                                 const std::optional<TabletSettings>& settings)
        : udpSocket(std::move(udpSocket)), controlTransport(this->udpSocket), sessionId(sessionId),
          senderRestartTimeout(senderRestartTimeout) {
        mailbox.reserve(MAX_FRAME_SIZE);
        if (settings) {
            session.emplace(controlTransport, *settings, [this] { return static_cast<std::uint32_t>(randomDevice()); });
        }
    }

    Result<std::unique_ptr<FrameReceiver>> FrameReceiver::open(const Endpoint& bindTo, std::uint32_t sessionId,
                                                               std::chrono::milliseconds senderRestartTimeout) {
        auto udpSocket = UDPSocket::open(bindTo);
        if (!udpSocket) {
            return std::unexpected(udpSocket.error());
        }
        static_cast<void>(udpSocket->setReceiveBufferSize(RECEIVE_BUFFER_SIZE));
        return std::unique_ptr<FrameReceiver>(
                new FrameReceiver(std::move(*udpSocket), sessionId, senderRestartTimeout, std::nullopt));
    }

    Result<std::unique_ptr<FrameReceiver>> FrameReceiver::openSession(const TabletSettings& settings) {
        auto udpSocket = UDPSocket::open(Endpoint{ANY_IP_ADDRESS, 0});
        if (!udpSocket) {
            return std::unexpected(udpSocket.error());
        }
        static_cast<void>(udpSocket->setReceiveBufferSize(RECEIVE_BUFFER_SIZE));
        return std::unique_ptr<FrameReceiver>(
                new FrameReceiver(std::move(*udpSocket), 0, DEFAULT_SENDER_RESTART_TIMEOUT, settings));
    }

    FrameReceiver::~FrameReceiver() {
        stop();
    }

    Result<Endpoint> FrameReceiver::localEndpoint() const {
        return udpSocket.localEndpoint();
    }

    void FrameReceiver::start() {
        if (receiveThread.joinable()) {
            return;
        }
        resetReassembler();
        mailbox.reset();
        lastCompletion = std::chrono::steady_clock::now();
        stopRequested.store(false, std::memory_order_relaxed);
        receiveThread = std::thread([this] { run(); });
    }

    void FrameReceiver::stop() noexcept {
        stopRequested.store(true, std::memory_order_relaxed);
        if (receiveThread.joinable()) {
            receiveThread.join();
        }
        mailbox.close();
    }

    MailboxStatus FrameReceiver::acquire(ReassembledFrame& frame, std::chrono::milliseconds timeout) noexcept {
        std::span<const std::uint8_t> bytes;
        const MailboxStatus status = mailbox.acquire(frame, bytes, timeout);
        if (status == MailboxStatus::FRAME_READY) {
            frame.data = bytes;
        }
        return status;
    }

    void FrameReceiver::release() noexcept {
        mailbox.release();
    }

    FrameReceiverStatistics FrameReceiver::statistics() noexcept {
        return FrameReceiverStatistics{
                .packets = packets.load(std::memory_order_relaxed),
                .invalidPackets = invalidPackets.load(std::memory_order_relaxed),
                .ignoredPackets = ignoredPackets.load(std::memory_order_relaxed),
                .framesCompleted = framesCompleted.load(std::memory_order_relaxed),
                .framesDropped = framesDropped.load(std::memory_order_relaxed),
                .framesReplaced = mailbox.droppedFrameCount(),
                .senderRestarts = senderRestarts.load(std::memory_order_relaxed),
        };
    }

    void FrameReceiver::requestKeyframe() noexcept {
        keyframeWanted.store(true, std::memory_order_relaxed);
    }

    void FrameReceiver::reconnect() noexcept {
        reconnectWanted.store(true, std::memory_order_relaxed);
    }

    FrameReceiverSessionStatus FrameReceiver::sessionStatus() const noexcept {
        FrameReceiverSessionStatus status;
        status.state = statusState.load(std::memory_order_acquire);
        status.rejectReason = statusRejectReason.load(std::memory_order_relaxed);
        status.sessionId = statusSessionId.load(std::memory_order_relaxed);
        if (const std::int64_t roundTripUs = statusRoundTripUs.load(std::memory_order_relaxed); roundTripUs >= 0) {
            status.roundTripTime = std::chrono::microseconds(roundTripUs);
        }
        status.stream = StreamInfo{.width = statusWidth.load(std::memory_order_relaxed),
                                   .height = statusHeight.load(std::memory_order_relaxed),
                                   .fps = statusFps.load(std::memory_order_relaxed),
                                   .codec = statusCodec.load(std::memory_order_relaxed),
                                   .features = statusFeatures.load(std::memory_order_relaxed)};
        status.sessionCount = statusSessionCount.load(std::memory_order_relaxed);
        return status;
    }

    void FrameReceiver::run() noexcept {
        Endpoint from;
        const auto pollInterval = session ? SESSION_POLL_INTERVAL : RECEIVE_POLL_INTERVAL;
        if (session) {
            session->start(std::chrono::steady_clock::now());
            lastSessionCount = session->sessionCount();
            publishSessionStatus();
        }
        while (!stopRequested.load(std::memory_order_relaxed)) {
            const ReceiveResult result = udpSocket.receiveFrom(packetBuffer, from, pollInterval);
            const auto now = std::chrono::steady_clock::now();
            switch (result.status) {
                case ReceiveStatus::RECEIVED:
                    packets.fetch_add(1, std::memory_order_relaxed);
                    handlePacket(std::span<const std::uint8_t>(packetBuffer).first(result.size), from,
                                 result.arrivalTime);
                    break;
                case ReceiveStatus::TRUNCATED:
                    packets.fetch_add(1, std::memory_order_relaxed);
                    invalidPackets.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ReceiveStatus::NO_DATAGRAM:
                    reassembler.expire(now);
                    break;
                case ReceiveStatus::RECEIVE_FAILED:
                    std::this_thread::sleep_for(pollInterval);
                    break;
            }
            noteDroppedFrames(now);
            if (session) {
                pollSession(now);
            }
        }
        if (session) {
            closeSession();
        }
        mailbox.close();
    }

    void FrameReceiver::handlePacket(std::span<const std::uint8_t> packet, const Endpoint& from,
                                     std::chrono::steady_clock::time_point arrival) noexcept {
        CommonHeader header;
        const bool validHeader = header.read(packet) && isValidHeader(header);
        if (session) {
            const SessionPacket kind = session->handlePacket(packet, from, arrival);
            if (kind == SessionPacket::CONTROL) {
                return;
            }
            if (kind == SessionPacket::IGNORED) {
                (validHeader ? ignoredPackets : invalidPackets).fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (!validHeader) {
            invalidPackets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint32_t expectedSessionId = session ? session->sessionId() : sessionId;
        if (header.type != static_cast<std::uint8_t>(PacketType::VIDEO_FRAGMENT) ||
            header.sessionId != expectedSessionId) {
            ignoredPackets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        VideoFragment fragment;
        if (!fragment.read(packet)) {
            invalidPackets.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        FragmentStatus status = reassembler.accept(fragment, arrival);
        if (status == FragmentStatus::TOO_OLD && arrival - lastCompletion >= senderRestartTimeout) {
            resetReassembler();
            senderRestarts.fetch_add(1, std::memory_order_relaxed);
            lastCompletion = arrival;
            status = reassembler.accept(fragment, arrival);
        }
        if (status == FragmentStatus::INVALID) {
            invalidPackets.fetch_add(1, std::memory_order_relaxed);
        } else if (status == FragmentStatus::FRAME_COMPLETE) {
            publishCompletedFrame(arrival);
        }
    }

    void FrameReceiver::publishCompletedFrame(std::chrono::steady_clock::time_point now) noexcept {
        ReassembledFrame frame = reassembler.completedFrame();
        const std::size_t frameBytes = frame.data.size();
        std::ranges::copy(frame.data, mailbox.beginWrite(frameBytes));
        frame.data = {};
        mailbox.publish(frame, frameBytes);
        lastCompletion = now;
        framesCompleted.fetch_add(1, std::memory_order_relaxed);
        if (session) {
            session->frameCompleted();
            if (frame.keyframe) {
                session->keyframeReceived();
            }
        }
    }

    void FrameReceiver::resetReassembler() noexcept {
        framesDroppedBeforeReset += reassembler.framesDropped();
        reassembler.reset();
    }

    void FrameReceiver::noteDroppedFrames(std::chrono::steady_clock::time_point now) noexcept {
        const std::uint64_t total = framesDroppedBeforeReset + reassembler.framesDropped();
        framesDropped.store(total, std::memory_order_relaxed);
        const std::uint64_t newlyDropped = total - framesDroppedReported;
        framesDroppedReported = total;
        if (!session || newlyDropped == 0) {
            return;
        }
        for (std::uint64_t i = 0; i < newlyDropped; ++i) {
            session->frameDropped();
        }
        if (session->stream().codec == Codec::H264) {
            session->requestKeyframe(now);
        }
    }

    void FrameReceiver::pollSession(std::chrono::steady_clock::time_point now) noexcept {
        if (keyframeWanted.exchange(false, std::memory_order_relaxed)) {
            session->requestKeyframe(now);
        }
        const TabletState state = session->state();
        if (reconnectWanted.exchange(false, std::memory_order_relaxed) &&
            (state == TabletState::REJECTED || state == TabletState::ENDED_BY_HOST)) {
            session->start(now);
        }
        session->poll(now);
        if (session->sessionCount() != lastSessionCount) {
            lastSessionCount = session->sessionCount();
            resetReassembler();
            lastCompletion = now;
        }
        publishSessionStatus();
    }

    void FrameReceiver::closeSession() noexcept {
        const auto closeTime = std::chrono::steady_clock::now();
        session->close(closeTime);
        const auto byeDeadline = closeTime + BYE_WAIT_LIMIT;
        while (session->closing() && std::chrono::steady_clock::now() < byeDeadline) {
            std::this_thread::sleep_for(BYE_WAIT_STEP);
            session->poll(std::chrono::steady_clock::now());
        }
        publishSessionStatus();
    }

    void FrameReceiver::publishSessionStatus() noexcept {
        statusRejectReason.store(session->rejectReason(), std::memory_order_relaxed);
        statusSessionId.store(session->sessionId(), std::memory_order_relaxed);
        const auto roundTripTime = session->roundTripTime();
        statusRoundTripUs.store(roundTripTime ? roundTripTime->count() : -1, std::memory_order_relaxed);
        const StreamInfo& stream = session->stream();
        statusWidth.store(stream.width, std::memory_order_relaxed);
        statusHeight.store(stream.height, std::memory_order_relaxed);
        statusFps.store(stream.fps, std::memory_order_relaxed);
        statusCodec.store(stream.codec, std::memory_order_relaxed);
        statusFeatures.store(stream.features, std::memory_order_relaxed);
        statusSessionCount.store(session->sessionCount(), std::memory_order_relaxed);
        statusState.store(session->state(), std::memory_order_release);
    }
}
