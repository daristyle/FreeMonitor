#include "FrameReceiver.hpp"

#include <algorithm>
#include <utility>

namespace fm {
    namespace {
        constexpr auto RELAXED = std::memory_order_relaxed;

        bool isValidHeader(const CommonHeader& header) noexcept {
            return header.magic == MAGIC && header.version == PROTOCOL_VERSION && header.headerReserved == 0;
        }
    }

    FrameReceiver::FrameReceiver(UDPSocket udpSocket, std::uint32_t sessionId,
                                 std::chrono::milliseconds senderRestartTimeout)
        : udpSocket(std::move(udpSocket)), sessionId(sessionId), senderRestartTimeout(senderRestartTimeout) {
        mailbox.reserve(MAX_FRAME_SIZE);
    }

    Result<std::unique_ptr<FrameReceiver>> FrameReceiver::open(const Endpoint& bindTo, std::uint32_t sessionId,
                                                               std::chrono::milliseconds senderRestartTimeout) {
        auto udpSocket = UDPSocket::open(bindTo);
        if (!udpSocket) {
            return std::unexpected(udpSocket.error());
        }
        static_cast<void>(udpSocket->setReceiveBufferSize(RECEIVE_BUFFER_SIZE));
        return std::unique_ptr<FrameReceiver>(
                new FrameReceiver(std::move(*udpSocket), sessionId, senderRestartTimeout));
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
        stopRequested.store(false, RELAXED);
        receiveThread = std::thread([this] { run(); });
    }

    void FrameReceiver::stop() noexcept {
        stopRequested.store(true, RELAXED);
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
                .packets = packets.load(RELAXED),
                .invalidPackets = invalidPackets.load(RELAXED),
                .ignoredPackets = ignoredPackets.load(RELAXED),
                .framesCompleted = framesCompleted.load(RELAXED),
                .framesDropped = framesDropped.load(RELAXED),
                .framesReplaced = mailbox.droppedFrameCount(),
                .senderRestarts = senderRestarts.load(RELAXED),
        };
    }

    void FrameReceiver::run() noexcept {
        Endpoint from;
        while (!stopRequested.load(RELAXED)) {
            const ReceiveResult result = udpSocket.receiveFrom(packetBuffer, from, RECEIVE_POLL_INTERVAL);
            const auto now = std::chrono::steady_clock::now();
            switch (result.status) {
                case ReceiveStatus::RECEIVED:
                    packets.fetch_add(1, RELAXED);
                    handlePacket(std::span<const std::uint8_t>(packetBuffer).first(result.size), now);
                    break;
                case ReceiveStatus::TRUNCATED:
                    packets.fetch_add(1, RELAXED);
                    invalidPackets.fetch_add(1, RELAXED);
                    break;
                case ReceiveStatus::NO_DATAGRAM:
                    reassembler.expire(now);
                    break;
                case ReceiveStatus::RECEIVE_FAILED:
                    std::this_thread::sleep_for(RECEIVE_POLL_INTERVAL);
                    break;
            }
            framesDropped.store(framesDroppedBeforeReset + reassembler.framesDropped(), RELAXED);
        }
        mailbox.close();
    }

    void FrameReceiver::handlePacket(std::span<const std::uint8_t> packet,
                                     std::chrono::steady_clock::time_point now) noexcept {
        CommonHeader header;
        if (!header.read(packet) || !isValidHeader(header)) {
            invalidPackets.fetch_add(1, RELAXED);
            return;
        }
        if (header.type != static_cast<std::uint8_t>(PacketType::VIDEO_FRAGMENT) || header.sessionId != sessionId) {
            ignoredPackets.fetch_add(1, RELAXED);
            return;
        }
        VideoFragment fragment;
        if (!fragment.read(packet)) {
            invalidPackets.fetch_add(1, RELAXED);
            return;
        }

        FragmentStatus status = reassembler.accept(fragment, now);
        if (status == FragmentStatus::TOO_OLD && now - lastCompletion >= senderRestartTimeout) {
            resetReassembler();
            senderRestarts.fetch_add(1, RELAXED);
            lastCompletion = now;
            status = reassembler.accept(fragment, now);
        }
        if (status == FragmentStatus::INVALID) {
            invalidPackets.fetch_add(1, RELAXED);
        } else if (status == FragmentStatus::FRAME_COMPLETE) {
            publishCompletedFrame(now);
        }
    }

    void FrameReceiver::publishCompletedFrame(std::chrono::steady_clock::time_point now) noexcept {
        ReassembledFrame frame = reassembler.completedFrame();
        const std::size_t frameBytes = frame.data.size();
        std::ranges::copy(frame.data, mailbox.beginWrite(frameBytes));
        frame.data = {};
        mailbox.publish(frame, frameBytes);
        lastCompletion = now;
        framesCompleted.fetch_add(1, RELAXED);
    }

    void FrameReceiver::resetReassembler() noexcept {
        framesDroppedBeforeReset += reassembler.framesDropped();
        reassembler.reset();
    }
}
