#include "VideoSender.hpp"

#include "Timestamp.hpp"

#include <span>

namespace fm {
    VideoSender::VideoSender(UDPSocket& udpSocket, std::uint32_t firstFrameId)
        : udpSocket(udpSocket), nextFrameId(firstFrameId) {}

    FrameSendResult VideoSender::send(const EncodedFrame& frame, Codec codec, Session& session) noexcept {
        FrameSendResult result;
        if (!fragmenter.begin(frame.data, nextFrameId, codec, frame.keyframe, toTimestampUs(frame.captureTime))) {
            result.status = FrameSendStatus::INVALID_FRAME;
            return result;
        }
        result.frameId = nextFrameId;
        ++nextFrameId;

        const std::uint16_t fragmentCount = fragmenter.fragmentCount();
        for (std::uint16_t fragIndex = 0; fragIndex < fragmentCount; ++fragIndex) {
            const std::size_t packetSize =
                    fragmenter.write(fragIndex, session.sessionId, session.nextSeq,
                                     toTimestampUs(std::chrono::steady_clock::now()), packetBuffer);
            if (packetSize == 0) {
                result.status = FrameSendStatus::SEND_FAILED;
                return result;
            }

            const SendStatus sendStatus =
                    udpSocket.sendTo(std::span<const std::uint8_t>(packetBuffer).first(packetSize), session.peer);
            if (sendStatus != SendStatus::SENT) {
                result.status = sendStatus == SendStatus::WOULD_BLOCK ? FrameSendStatus::WOULD_BLOCK
                                                                      : FrameSendStatus::SEND_FAILED;
                return result;
            }
            ++session.nextSeq;
            ++result.packetsSent;
            result.bytesSent += packetSize;
        }
        return result;
    }
}
