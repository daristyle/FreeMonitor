#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace fm {
    constexpr std::uint16_t MAGIC = 0x464D;
    constexpr std::uint8_t PROTOCOL_VERSION = 1;
    constexpr std::uint32_t MAX_PACKET_SIZE = 1400;
    constexpr std::uint32_t MAX_FRAME_SIZE = 4194304;
    constexpr std::uint16_t HOST_PORT = 50500;
    constexpr std::uint16_t DISCOVERY_PORT = 50501;
    constexpr std::uint32_t FRAGMENT_TIMEOUT_MS = 100;
    constexpr std::uint32_t CURSOR_POS_MIN_INTERVAL_MS = 4;
    constexpr std::uint32_t CURSOR_POS_REPEAT_INTERVAL_MS = 250;
    constexpr std::uint32_t RELIABLE_RESEND_INTERVAL_MS = 100;
    constexpr std::uint32_t HELLO_RESEND_INTERVAL_MS = 500;
    constexpr std::uint32_t HEARTBEAT_INTERVAL_MS = 500;
    constexpr std::uint32_t SESSION_TIMEOUT_MS = 3000;
    constexpr std::uint32_t BYE_REPEAT_INTERVAL_MS = 20;
    constexpr std::uint32_t KEYFRAME_REQUEST_IGNORE_MS = 100;
    constexpr std::uint32_t KEYFRAME_REQUEST_REPEAT_INTERVAL_MS = 200;
    constexpr std::uint32_t SHAPE_REQUEST_REPEAT_INTERVAL_MS = 500;
    constexpr std::uint32_t STATS_INTERVAL_MS = 1000;
    constexpr std::uint32_t DEVELOPMENT_KEYFRAME_INTERVAL_MS = 1000;
    constexpr std::uint32_t MAX_INCOMPLETE_FRAMES = 4;
    constexpr std::uint32_t MIN_SHAPE_CACHE_SIZE = 16;
    constexpr std::uint32_t BYE_REPEAT_COUNT = 3;
    constexpr std::uint32_t MAX_FRAGMENT_PAYLOAD = 1352;
    constexpr std::uint32_t MAX_SHAPE_PAYLOAD = 1360;

    enum class PacketType : std::uint8_t {
        VIDEO_FRAGMENT = 0x01,
        CURSOR_POS = 0x10,
        CURSOR_SHAPE = 0x11,
        HELLO = 0x20,
        WELCOME = 0x21,
        ACK = 0x22,
        HEARTBEAT = 0x23,
        KEYFRAME_REQUEST = 0x24,
        SHAPE_REQUEST = 0x25,
        STATS = 0x26,
        BYE = 0x27,
        REJECT = 0x28,
    };

    enum class Codec : std::uint8_t {
        JPEG = 0,
        H264 = 1,
    };

    enum class RejectReason : std::uint8_t {
        UNSUPPORTED_VERSION = 1,
        NO_COMMON_CODEC = 2,
        INVALID_PARAMETERS = 3,
    };

    struct FragmentFlags {
        static constexpr std::uint8_t KEYFRAME = 0x01;
    };

    struct CursorFlags {
        static constexpr std::uint8_t VISIBLE = 0x01;
    };

    struct CodecFlags {
        static constexpr std::uint8_t JPEG = 0x01;
        static constexpr std::uint8_t H264 = 0x02;
    };

    struct FeatureFlags {
        static constexpr std::uint8_t CURSOR_OVERLAY = 0x01;
    };

    template<typename T>
    constexpr void writeBigEndian(std::uint8_t* destination, T value) noexcept {
        auto bits = static_cast<std::make_unsigned_t<T>>(value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            destination[sizeof(T) - 1 - index] = static_cast<std::uint8_t>(bits >> (8 * index));
        }
    }

    template<typename T>
    constexpr T readBigEndian(const std::uint8_t* source) noexcept {
        std::make_unsigned_t<T> bits = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bits = static_cast<std::make_unsigned_t<T>>((bits << 8) | source[index]);
        }
        return static_cast<T>(bits);
    }

    struct CommonHeader {
        static constexpr std::size_t OFFSET = 0;
        static constexpr std::size_t SIZE = 24;

        std::uint16_t magic = MAGIC;
        std::uint8_t version = PROTOCOL_VERSION;
        std::uint8_t type = 0;
        std::uint32_t sessionId = 0;
        std::uint32_t seq = 0;
        std::uint32_t headerReserved = 0;
        std::uint64_t timestampUs = 0;

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < OFFSET + SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            magic = readBigEndian<std::uint16_t>(source);
            version = readBigEndian<std::uint8_t>(source + 2);
            type = readBigEndian<std::uint8_t>(source + 3);
            sessionId = readBigEndian<std::uint32_t>(source + 4);
            seq = readBigEndian<std::uint32_t>(source + 8);
            headerReserved = readBigEndian<std::uint32_t>(source + 12);
            timestampUs = readBigEndian<std::uint64_t>(source + 16);
            return true;
        }
    };

    constexpr CommonHeader makeCommonHeader(PacketType packetType) noexcept {
        CommonHeader commonHeader;
        commonHeader.type = static_cast<std::uint8_t>(packetType);
        return commonHeader;
    }

    struct ControlHeader {
        static constexpr std::size_t OFFSET = 24;
        static constexpr std::size_t SIZE = 8;

        std::uint32_t msgId = 0;
        std::uint32_t controlReserved = 0;

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < OFFSET + SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            msgId = readBigEndian<std::uint32_t>(source + 24);
            controlReserved = readBigEndian<std::uint32_t>(source + 28);
            return true;
        }
    };

    struct VideoFragment {
        static constexpr std::size_t SIZE = 48;

        CommonHeader commonHeader = makeCommonHeader(PacketType::VIDEO_FRAGMENT);
        std::uint32_t frameId = 0;
        std::uint16_t fragIndex = 0;
        std::uint16_t fragCount = 0;
        std::uint32_t frameSize = 0;
        std::uint8_t codec = 0;
        std::uint8_t flags = 0;
        std::uint16_t reserved = 0;
        std::uint64_t captureTimestampUs = 0;
        std::span<const std::uint8_t> payload;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE || buffer.size() - SIZE < payload.size()) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, frameId);
            writeBigEndian(destination + 28, fragIndex);
            writeBigEndian(destination + 30, fragCount);
            writeBigEndian(destination + 32, frameSize);
            writeBigEndian(destination + 36, codec);
            writeBigEndian(destination + 37, flags);
            writeBigEndian(destination + 38, reserved);
            writeBigEndian(destination + 40, captureTimestampUs);
            std::ranges::copy(payload, destination + SIZE);
            return SIZE + payload.size();
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            frameId = readBigEndian<std::uint32_t>(source + 24);
            fragIndex = readBigEndian<std::uint16_t>(source + 28);
            fragCount = readBigEndian<std::uint16_t>(source + 30);
            frameSize = readBigEndian<std::uint32_t>(source + 32);
            codec = readBigEndian<std::uint8_t>(source + 36);
            flags = readBigEndian<std::uint8_t>(source + 37);
            reserved = readBigEndian<std::uint16_t>(source + 38);
            captureTimestampUs = readBigEndian<std::uint64_t>(source + 40);
            payload = buffer.subspan(SIZE);
            return true;
        }
    };

    struct CursorPos {
        static constexpr std::size_t SIZE = 40;

        CommonHeader commonHeader = makeCommonHeader(PacketType::CURSOR_POS);
        std::uint32_t shapeId = 0;
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::uint8_t flags = 0;
        std::uint8_t reserved0 = 0;
        std::uint16_t reserved1 = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, shapeId);
            writeBigEndian(destination + 28, x);
            writeBigEndian(destination + 32, y);
            writeBigEndian(destination + 36, flags);
            writeBigEndian(destination + 37, reserved0);
            writeBigEndian(destination + 38, reserved1);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            shapeId = readBigEndian<std::uint32_t>(source + 24);
            x = readBigEndian<std::int32_t>(source + 28);
            y = readBigEndian<std::int32_t>(source + 32);
            flags = readBigEndian<std::uint8_t>(source + 36);
            reserved0 = readBigEndian<std::uint8_t>(source + 37);
            reserved1 = readBigEndian<std::uint16_t>(source + 38);
            return true;
        }
    };

    struct CursorShape {
        static constexpr std::size_t SIZE = 40;

        CommonHeader commonHeader = makeCommonHeader(PacketType::CURSOR_SHAPE);
        std::uint32_t shapeId = 0;
        std::uint16_t fragIndex = 0;
        std::uint16_t fragCount = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t hotspotX = 0;
        std::uint16_t hotspotY = 0;
        std::span<const std::uint8_t> payload;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE || buffer.size() - SIZE < payload.size()) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, shapeId);
            writeBigEndian(destination + 28, fragIndex);
            writeBigEndian(destination + 30, fragCount);
            writeBigEndian(destination + 32, width);
            writeBigEndian(destination + 34, height);
            writeBigEndian(destination + 36, hotspotX);
            writeBigEndian(destination + 38, hotspotY);
            std::ranges::copy(payload, destination + SIZE);
            return SIZE + payload.size();
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            shapeId = readBigEndian<std::uint32_t>(source + 24);
            fragIndex = readBigEndian<std::uint16_t>(source + 28);
            fragCount = readBigEndian<std::uint16_t>(source + 30);
            width = readBigEndian<std::uint16_t>(source + 32);
            height = readBigEndian<std::uint16_t>(source + 34);
            hotspotX = readBigEndian<std::uint16_t>(source + 36);
            hotspotY = readBigEndian<std::uint16_t>(source + 38);
            payload = buffer.subspan(SIZE);
            return true;
        }
    };

    struct Hello {
        static constexpr std::size_t SIZE = 40;

        CommonHeader commonHeader = makeCommonHeader(PacketType::HELLO);
        ControlHeader controlHeader;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t refreshHz = 0;
        std::uint8_t codecs = 0;
        std::uint8_t features = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            writeBigEndian(destination + 32, width);
            writeBigEndian(destination + 34, height);
            writeBigEndian(destination + 36, refreshHz);
            writeBigEndian(destination + 38, codecs);
            writeBigEndian(destination + 39, features);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            width = readBigEndian<std::uint16_t>(source + 32);
            height = readBigEndian<std::uint16_t>(source + 34);
            refreshHz = readBigEndian<std::uint16_t>(source + 36);
            codecs = readBigEndian<std::uint8_t>(source + 38);
            features = readBigEndian<std::uint8_t>(source + 39);
            return true;
        }
    };

    struct Welcome {
        static constexpr std::size_t SIZE = 40;

        CommonHeader commonHeader = makeCommonHeader(PacketType::WELCOME);
        ControlHeader controlHeader;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t fps = 0;
        std::uint8_t codec = 0;
        std::uint8_t features = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            writeBigEndian(destination + 32, width);
            writeBigEndian(destination + 34, height);
            writeBigEndian(destination + 36, fps);
            writeBigEndian(destination + 38, codec);
            writeBigEndian(destination + 39, features);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            width = readBigEndian<std::uint16_t>(source + 32);
            height = readBigEndian<std::uint16_t>(source + 34);
            fps = readBigEndian<std::uint16_t>(source + 36);
            codec = readBigEndian<std::uint8_t>(source + 38);
            features = readBigEndian<std::uint8_t>(source + 39);
            return true;
        }
    };

    struct ACK {
        static constexpr std::size_t SIZE = 36;

        CommonHeader commonHeader = makeCommonHeader(PacketType::ACK);
        ControlHeader controlHeader;
        std::uint32_t ackedMsgId = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            writeBigEndian(destination + 32, ackedMsgId);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            ackedMsgId = readBigEndian<std::uint32_t>(source + 32);
            return true;
        }
    };

    struct Heartbeat {
        static constexpr std::size_t SIZE = 48;

        CommonHeader commonHeader = makeCommonHeader(PacketType::HEARTBEAT);
        ControlHeader controlHeader;
        std::uint64_t echoTimestampUs = 0;
        std::uint32_t echoDelayUs = 0;
        std::uint32_t reserved = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            writeBigEndian(destination + 32, echoTimestampUs);
            writeBigEndian(destination + 40, echoDelayUs);
            writeBigEndian(destination + 44, reserved);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            echoTimestampUs = readBigEndian<std::uint64_t>(source + 32);
            echoDelayUs = readBigEndian<std::uint32_t>(source + 40);
            reserved = readBigEndian<std::uint32_t>(source + 44);
            return true;
        }
    };

    struct KeyframeRequest {
        static constexpr std::size_t SIZE = 32;

        CommonHeader commonHeader = makeCommonHeader(PacketType::KEYFRAME_REQUEST);
        ControlHeader controlHeader;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            return true;
        }
    };

    struct ShapeRequest {
        static constexpr std::size_t SIZE = 36;

        CommonHeader commonHeader = makeCommonHeader(PacketType::SHAPE_REQUEST);
        ControlHeader controlHeader;
        std::uint32_t shapeId = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            writeBigEndian(destination + 32, shapeId);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            shapeId = readBigEndian<std::uint32_t>(source + 32);
            return true;
        }
    };

    struct Stats {
        static constexpr std::size_t SIZE = 52;

        CommonHeader commonHeader = makeCommonHeader(PacketType::STATS);
        ControlHeader controlHeader;
        std::uint32_t packetsReceived = 0;
        std::uint32_t packetsLost = 0;
        std::uint32_t framesCompleted = 0;
        std::uint32_t framesDropped = 0;
        std::uint32_t bytesReceived = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            writeBigEndian(destination + 32, packetsReceived);
            writeBigEndian(destination + 36, packetsLost);
            writeBigEndian(destination + 40, framesCompleted);
            writeBigEndian(destination + 44, framesDropped);
            writeBigEndian(destination + 48, bytesReceived);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            packetsReceived = readBigEndian<std::uint32_t>(source + 32);
            packetsLost = readBigEndian<std::uint32_t>(source + 36);
            framesCompleted = readBigEndian<std::uint32_t>(source + 40);
            framesDropped = readBigEndian<std::uint32_t>(source + 44);
            bytesReceived = readBigEndian<std::uint32_t>(source + 48);
            return true;
        }
    };

    struct Bye {
        static constexpr std::size_t SIZE = 32;

        CommonHeader commonHeader = makeCommonHeader(PacketType::BYE);
        ControlHeader controlHeader;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            return true;
        }
    };

    struct Reject {
        static constexpr std::size_t SIZE = 36;

        CommonHeader commonHeader = makeCommonHeader(PacketType::REJECT);
        ControlHeader controlHeader;
        std::uint8_t reason = 0;
        std::uint8_t version = 0;
        std::uint16_t reserved = 0;

        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {
            if (buffer.size() < SIZE) {
                return 0;
            }
            std::uint8_t* destination = buffer.data();
            writeBigEndian(destination, commonHeader.magic);
            writeBigEndian(destination + 2, commonHeader.version);
            writeBigEndian(destination + 3, commonHeader.type);
            writeBigEndian(destination + 4, commonHeader.sessionId);
            writeBigEndian(destination + 8, commonHeader.seq);
            writeBigEndian(destination + 12, commonHeader.headerReserved);
            writeBigEndian(destination + 16, commonHeader.timestampUs);
            writeBigEndian(destination + 24, controlHeader.msgId);
            writeBigEndian(destination + 28, controlHeader.controlReserved);
            writeBigEndian(destination + 32, reason);
            writeBigEndian(destination + 33, version);
            writeBigEndian(destination + 34, reserved);
            return SIZE;
        }

        bool read(std::span<const std::uint8_t> buffer) noexcept {
            if (buffer.size() < SIZE) {
                return false;
            }
            const std::uint8_t* source = buffer.data();
            commonHeader.magic = readBigEndian<std::uint16_t>(source);
            commonHeader.version = readBigEndian<std::uint8_t>(source + 2);
            commonHeader.type = readBigEndian<std::uint8_t>(source + 3);
            commonHeader.sessionId = readBigEndian<std::uint32_t>(source + 4);
            commonHeader.seq = readBigEndian<std::uint32_t>(source + 8);
            commonHeader.headerReserved = readBigEndian<std::uint32_t>(source + 12);
            commonHeader.timestampUs = readBigEndian<std::uint64_t>(source + 16);
            controlHeader.msgId = readBigEndian<std::uint32_t>(source + 24);
            controlHeader.controlReserved = readBigEndian<std::uint32_t>(source + 28);
            reason = readBigEndian<std::uint8_t>(source + 32);
            version = readBigEndian<std::uint8_t>(source + 33);
            reserved = readBigEndian<std::uint16_t>(source + 34);
            return true;
        }
    };
}
