#include "CursorSender.hpp"

#include "CursorPackets.hpp"
#include "Timestamp.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace fm {
    CursorSender::CursorSender(UDPSocket& udpSocket, CursorSource& source,
                               std::optional<std::chrono::milliseconds> shapeRepeatInterval)
        : udpSocket(udpSocket), source(source), shapeRepeatInterval(shapeRepeatInterval) {}

    CursorSender::~CursorSender() {
        stop();
    }

    void CursorSender::start() {
        if (thread.joinable()) {
            return;
        }
        stopRequested.store(false, std::memory_order_relaxed);
        thread = std::thread([this] { run(); });
    }

    void CursorSender::stop() noexcept {
        if (!thread.joinable()) {
            return;
        }
        stopRequested.store(true, std::memory_order_relaxed);
        source.wake();
        thread.join();
    }

    void CursorSender::setTarget(std::optional<CursorTarget> newTarget) {
        {
            const std::lock_guard lock(targetMutex);
            pendingTarget = std::move(newTarget);
        }
        targetChanged.store(true, std::memory_order_release);
        source.wake();
    }

    void CursorSender::requestShape(std::uint32_t shapeId) noexcept {
        requestedShape.store(shapeId, std::memory_order_relaxed);
        source.wake();
    }

    bool CursorSender::lost() const noexcept {
        return sourceLost.load(std::memory_order_relaxed);
    }

    CursorSendCounts CursorSender::counts() const noexcept {
        return {.packets = packetsSent.load(std::memory_order_relaxed),
                .bytes = bytesSent.load(std::memory_order_relaxed)};
    }

    void CursorSender::update(const CursorState& state) noexcept {
        latest = state;
    }

    CursorSender::Clock::time_point CursorSender::sendDue(Clock::time_point now) noexcept {
        if (targetChanged.exchange(false, std::memory_order_acquire)) {
            const std::lock_guard lock(targetMutex);
            target = pendingTarget;
            positionSent = false;
            sentShapeId = 0;
        }
        const std::uint32_t requested = requestedShape.exchange(0, std::memory_order_relaxed);
        if (!target || latest.imageId == 0) {
            return now + POSITION_REPEAT_INTERVAL;
        }

        const bool shapeRepeatDue = shapeRepeatInterval && now - lastShapeTime >= *shapeRepeatInterval;
        if (sentShapeId != latest.imageId || requested == latest.imageId || shapeRepeatDue) {
            sendShape(now);
        }
        if (sentShapeId != latest.imageId) {
            return now + MIN_POSITION_INTERVAL;
        }

        const bool allowed = !positionSent || now - lastPositionTime >= MIN_POSITION_INTERVAL;
        const bool repeatDue = positionSent && now - lastPositionTime >= POSITION_REPEAT_INTERVAL;
        if ((positionPending() && allowed) || repeatDue) {
            sendPosition(now);
        }

        Clock::time_point deadline = lastPositionTime + POSITION_REPEAT_INTERVAL;
        if (positionPending()) {
            deadline = std::min(deadline, lastPositionTime + MIN_POSITION_INTERVAL);
        }
        if (shapeRepeatInterval) {
            deadline = std::min(deadline, lastShapeTime + *shapeRepeatInterval);
        }
        return deadline;
    }

    void CursorSender::run() noexcept {
        CursorState state;
        while (!stopRequested.load(std::memory_order_relaxed)) {
            const Clock::time_point now = Clock::now();
            const Clock::time_point deadline = sendDue(now);
            const auto timeout = std::clamp(std::chrono::ceil<std::chrono::milliseconds>(deadline - now),
                                            std::chrono::milliseconds{0}, POSITION_REPEAT_INTERVAL);
            switch (source.waitForChange(state, timeout)) {
                case CursorStatus::CHANGED:
                    update(state);
                    break;
                case CursorStatus::UNCHANGED:
                    break;
                case CursorStatus::CURSOR_LOST:
                    sourceLost.store(true, std::memory_order_relaxed);
                    return;
            }
        }
    }

    bool CursorSender::positionPending() const noexcept {
        return !positionSent || latest.x != sent.x || latest.y != sent.y || latest.visible != sent.visible ||
               latest.imageId != sent.imageId;
    }

    void CursorSender::sendShape(Clock::time_point now) noexcept {
        lastShapeTime = now;
        const CursorImage& image = source.image();
        const std::uint16_t fragCount = cursorShapeFragmentCount(image);
        if (fragCount == 0) {
            return;
        }
        for (std::uint16_t fragIndex = 0; fragIndex < fragCount; ++fragIndex) {
            const std::size_t size =
                    writeCursorShapeFragment(image, latest.imageId, fragIndex, target->sessionId, target->seq.next(),
                                             toTimestampUs(Clock::now()), packetBuffer);
            if (!sendPacket(size)) {
                return;
            }
        }
        sentShapeId = latest.imageId;
    }

    void CursorSender::sendPosition(Clock::time_point now) noexcept {
        lastPositionTime = now;
        const std::size_t size = writeCursorPos(latest, target->sessionId, target->seq.next(),
                                                toTimestampUs(Clock::now()), packetBuffer);
        if (sendPacket(size)) {
            sent = latest;
            positionSent = true;
        }
    }

    bool CursorSender::sendPacket(std::size_t size) noexcept {
        if (size == 0 || udpSocket.sendTo(std::span<const std::uint8_t>(packetBuffer).first(size), target->peer) !=
                                 SendStatus::SENT) {
            return false;
        }
        packetsSent.fetch_add(1, std::memory_order_relaxed);
        bytesSent.fetch_add(size, std::memory_order_relaxed);
        return true;
    }
}
