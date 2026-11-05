#pragma once

namespace fm {
    template<typename Message>
    std::size_t writeControlPacket(Message message, std::uint32_t sessionId, std::uint32_t seq,
                                   std::uint64_t timestampUs, std::span<std::uint8_t> buffer) noexcept {
        message.commonHeader.sessionId = sessionId;
        message.commonHeader.seq = seq;
        message.commonHeader.timestampUs = timestampUs;
        return message.write(buffer);
    }
}
