#include "ControlPackets.hpp"

namespace fm {
    namespace {
        constexpr std::size_t SEQ_OFFSET = 8;
        constexpr std::size_t TIMESTAMP_OFFSET = 16;
        constexpr std::uint8_t KNOWN_CODECS = CodecFlags::JPEG | CodecFlags::H264;
        constexpr std::uint8_t UNKNOWN_CODECS = static_cast<std::uint8_t>(~KNOWN_CODECS);

        std::size_t expectedSize(PacketType type) noexcept {
            switch (type) {
                case PacketType::HELLO:
                    return Hello::SIZE;
                case PacketType::WELCOME:
                    return Welcome::SIZE;
                case PacketType::ACK:
                    return ACK::SIZE;
                case PacketType::HEARTBEAT:
                    return Heartbeat::SIZE;
                case PacketType::KEYFRAME_REQUEST:
                    return KeyframeRequest::SIZE;
                case PacketType::SHAPE_REQUEST:
                    return ShapeRequest::SIZE;
                case PacketType::STATS:
                    return Stats::SIZE;
                case PacketType::BYE:
                    return Bye::SIZE;
                case PacketType::REJECT:
                    return Reject::SIZE;
                case PacketType::VIDEO_FRAGMENT:
                case PacketType::VIDEO_PARITY:
                case PacketType::CURSOR_POS:
                case PacketType::CURSOR_SHAPE:
                    break;
            }
            return 0;
        }

        bool isControlType(std::uint8_t type) noexcept {
            return type >= static_cast<std::uint8_t>(PacketType::HELLO) &&
                   type <= static_cast<std::uint8_t>(PacketType::REJECT);
        }

        bool reservedBodyFieldsAreZero(PacketType type, std::span<const std::uint8_t> packet) noexcept {
            switch (type) {
                case PacketType::HELLO: {
                    Hello hello;
                    return hello.read(packet) && (hello.codecs & UNKNOWN_CODECS) == 0;
                }
                case PacketType::HEARTBEAT: {
                    Heartbeat heartbeat;
                    return heartbeat.read(packet) && heartbeat.reserved == 0;
                }
                case PacketType::REJECT: {
                    Reject reject;
                    return reject.read(packet) && reject.reserved == 0;
                }
                default:
                    return true;
            }
        }
    }

    ControlPacketCheck checkControlPacket(std::span<const std::uint8_t> packet) noexcept {
        CommonHeader header;
        if (!header.read(packet) || header.magic != MAGIC || !isControlType(header.type) || header.sessionId == 0) {
            return ControlPacketCheck::INVALID;
        }
        const auto type = static_cast<PacketType>(header.type);
        if (header.version != PROTOCOL_VERSION) {
            if (type == PacketType::HELLO) {
                return ControlPacketCheck::UNSUPPORTED_VERSION;
            }
            if (type != PacketType::REJECT) {
                return ControlPacketCheck::INVALID;
            }
        }
        ControlHeader controlHeader;
        if (packet.size() != expectedSize(type) || header.headerReserved != 0 || !controlHeader.read(packet) ||
            controlHeader.controlReserved != 0 || (!isReliable(type) && controlHeader.msgId != 0) ||
            !reservedBodyFieldsAreZero(type, packet)) {
            return ControlPacketCheck::INVALID;
        }
        return ControlPacketCheck::VALID;
    }

    bool isMediaPacket(const CommonHeader& header) noexcept {
        const bool mediaType = header.type == static_cast<std::uint8_t>(PacketType::VIDEO_FRAGMENT) ||
                               header.type == static_cast<std::uint8_t>(PacketType::VIDEO_PARITY) ||
                               header.type == static_cast<std::uint8_t>(PacketType::CURSOR_POS) ||
                               header.type == static_cast<std::uint8_t>(PacketType::CURSOR_SHAPE);
        return mediaType && header.magic == MAGIC && header.version == PROTOCOL_VERSION && header.headerReserved == 0;
    }

    bool isReliable(PacketType type) noexcept {
        return type == PacketType::HELLO || type == PacketType::WELCOME;
    }

    void restampHeader(std::span<std::uint8_t> packet, std::uint32_t seq, std::uint64_t timestampUs) noexcept {
        if (packet.size() < CommonHeader::SIZE) {
            return;
        }
        writeBigEndian(packet.data() + SEQ_OFFSET, seq);
        writeBigEndian(packet.data() + TIMESTAMP_OFFSET, timestampUs);
    }
}