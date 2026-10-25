#pragma once

#include "Fragmenter.hpp"
#include "Frame.hpp"
#include "Packets.hpp"
#include "Session.hpp"
#include "UDPSocket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fm {
    enum class FrameSendStatus : std::uint8_t {
        FRAME_SENT,
        INVALID_FRAME,
        WOULD_BLOCK,
        SEND_FAILED,
    };

    struct FrameSendResult {
        FrameSendStatus status = FrameSendStatus::FRAME_SENT;
        std::optional<std::uint32_t> frameId;
        std::uint16_t packetsSent = 0;
        std::size_t bytesSent = 0;
    };

    class VideoSender {
    private:
        UDPSocket& udpSocket;
        Fragmenter fragmenter;
        std::array<std::uint8_t, MAX_PACKET_SIZE> packetBuffer{};
        std::uint32_t nextFrameId;

    public:
        VideoSender(UDPSocket& udpSocket, std::uint32_t firstFrameId);

        VideoSender(const VideoSender&) = delete;
        VideoSender& operator=(const VideoSender&) = delete;

        FrameSendResult send(const EncodedFrame& frame, Codec codec, Session& session) noexcept;
    };
}
