#include "ReliableDelivery.hpp"

#include "SerialNumber.hpp"

#include <algorithm>

namespace fm {
    namespace {
        constexpr std::size_t MSG_ID_OFFSET = 24;
    }

    void ReliableSender::reset() noexcept {
        first = 0;
        count = 0;
        nextMsgId = 1;
    }

    std::optional<std::uint32_t> ReliableSender::enqueue(std::span<const std::uint8_t> packet,
                                                         std::chrono::steady_clock::time_point now) noexcept {
        if (count == QUEUE_CAPACITY || packet.size() < CommonHeader::SIZE + ControlHeader::SIZE ||
            packet.size() > MAX_CONTROL_PACKET_SIZE) {
            return std::nullopt;
        }
        Message& message = queue[(first + count) % QUEUE_CAPACITY];
        std::ranges::copy(packet, message.bytes.begin());
        message.size = packet.size();
        message.msgId = nextMsgId;
        writeBigEndian(message.bytes.data() + MSG_ID_OFFSET, message.msgId);
        if (count == 0) {
            nextSendTime = now;
        }
        ++count;
        if (++nextMsgId == 0) {
            nextMsgId = 1;
        }
        return message.msgId;
    }

    std::span<std::uint8_t> ReliableSender::due(std::chrono::steady_clock::time_point now,
                                                std::chrono::milliseconds resendInterval) noexcept {
        if (count == 0 || now < nextSendTime) {
            return {};
        }
        nextSendTime = now + resendInterval;
        Message& message = queue[first];
        return std::span<std::uint8_t>(message.bytes).first(message.size);
    }

    bool ReliableSender::acknowledge(std::uint32_t msgId, std::chrono::steady_clock::time_point now) noexcept {
        if (count == 0 || queue[first].msgId != msgId) {
            return false;
        }
        first = (first + 1) % QUEUE_CAPACITY;
        --count;
        nextSendTime = now;
        return true;
    }

    bool ReliableSender::empty() const noexcept {
        return count == 0;
    }

    void ReliableReceiver::reset() noexcept {
        lastProcessedMsgId = 0;
        processedAny = false;
    }

    bool ReliableReceiver::shouldProcess(std::uint32_t msgId) noexcept {
        if (processedAny && !isNewer(msgId, lastProcessedMsgId)) {
            return false;
        }
        processedAny = true;
        lastProcessedMsgId = msgId;
        return true;
    }
}
