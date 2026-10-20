from dataclasses import dataclass

from freemonitor.Fragmenter import SERIAL_MODULUS, fragmentCount, fragmentPayloadSize
from freemonitor.Packets import (
    FRAGMENT_TIMEOUT_MS,
    MAGIC,
    MAX_FRAGMENT_PAYLOAD,
    MAX_FRAME_SIZE,
    MAX_INCOMPLETE_FRAMES,
    PROTOCOL_VERSION,
    Codec,
    FragmentFlags,
    PacketType,
    VideoFragment,
)

SERIAL_HALF = SERIAL_MODULUS // 2
FRAGMENT_TIMEOUT_NS = FRAGMENT_TIMEOUT_MS * 1_000_000
VALID_FRAGMENT_FLAGS = int(FragmentFlags.KEYFRAME)
VALID_CODECS = frozenset(Codec)


def serialDiff(newer: int, older: int) -> int:
    diff = (newer - older) % SERIAL_MODULUS
    return diff - SERIAL_MODULUS if diff >= SERIAL_HALF else diff


@dataclass(frozen=True)
class Frame:
    frameId: int
    codec: int
    flags: int
    captureTimestampUs: int
    data: bytes


@dataclass
class ReassemblerStats:
    deliveredFrames: int = 0
    discardedFrames: int = 0
    invalidPackets: int = 0
    duplicateFragments: int = 0
    staleFragments: int = 0


class PartialFrame:
    def __init__(self, fragment: VideoFragment, nowNs: int):
        self.fragCount = fragment.fragCount
        self.frameSize = fragment.frameSize
        self.codec = fragment.codec
        self.flags = fragment.flags
        self.captureTimestampUs = fragment.captureTimestampUs
        self.data = bytearray(fragment.frameSize)
        self.received = bytearray(fragment.fragCount)
        self.receivedCount = 0
        self.lastProgressNs = nowNs

    def matches(self, fragment: VideoFragment) -> bool:
        return (
            fragment.fragCount == self.fragCount
            and fragment.frameSize == self.frameSize
            and fragment.codec == self.codec
            and fragment.flags == self.flags
            and fragment.captureTimestampUs == self.captureTimestampUs
        )

    def add(self, fragment: VideoFragment, nowNs: int) -> bool:
        if self.received[fragment.fragIndex]:
            return False
        offset = fragment.fragIndex * MAX_FRAGMENT_PAYLOAD
        self.data[offset : offset + len(fragment.payload)] = fragment.payload
        self.received[fragment.fragIndex] = 1
        self.receivedCount += 1
        self.lastProgressNs = nowNs
        return True

    def isComplete(self) -> bool:
        return self.receivedCount == self.fragCount


class Reassembler:
    def __init__(self, sessionId: int = 0):
        self.sessionId = sessionId
        self.stats = ReassemblerStats()
        self.incompleteFrames: dict[int, PartialFrame] = {}
        self.lastDeliveredId: int | None = None

    def reset(self) -> None:
        self.incompleteFrames.clear()
        self.lastDeliveredId = None

    def push(self, datagram: bytes, nowNs: int) -> Frame | None:
        self.expire(nowNs)
        fragment = self.parse(datagram)
        if fragment is None:
            self.stats.invalidPackets += 1
            return None
        frameId = fragment.frameId
        if self.lastDeliveredId is not None and serialDiff(frameId, self.lastDeliveredId) <= 0:
            self.stats.staleFragments += 1
            return None
        partial = self.incompleteFrames.get(frameId)
        if partial is None:
            partial = self.startFrame(fragment, nowNs)
            if partial is None:
                self.stats.staleFragments += 1
                return None
        elif not partial.matches(fragment):
            self.stats.invalidPackets += 1
            return None
        if not partial.add(fragment, nowNs):
            self.stats.duplicateFragments += 1
            return None
        if not partial.isComplete():
            return None
        return self.deliver(frameId, partial)

    def expire(self, nowNs: int) -> None:
        expiredIds = [
            frameId
            for frameId, partial in self.incompleteFrames.items()
            if nowNs - partial.lastProgressNs >= FRAGMENT_TIMEOUT_NS
        ]
        self.discard(expiredIds)

    def parse(self, datagram: bytes) -> VideoFragment | None:
        if len(datagram) < VideoFragment.SIZE:
            return None
        fragment = VideoFragment.unpack(datagram)
        header = fragment.commonHeader
        if header.magic != MAGIC or header.version != PROTOCOL_VERSION or header.type != PacketType.VIDEO_FRAGMENT:
            return None
        if header.sessionId != self.sessionId or header.headerReserved != 0 or fragment.reserved != 0:
            return None
        if fragment.flags & ~VALID_FRAGMENT_FLAGS or fragment.codec not in VALID_CODECS:
            return None
        if not 1 <= fragment.frameSize <= MAX_FRAME_SIZE:
            return None
        if fragment.fragCount != fragmentCount(fragment.frameSize) or fragment.fragIndex >= fragment.fragCount:
            return None
        if len(fragment.payload) != fragmentPayloadSize(fragment.frameSize, fragment.fragIndex):
            return None
        return fragment

    def startFrame(self, fragment: VideoFragment, nowNs: int) -> PartialFrame | None:
        frameId = fragment.frameId
        if len(self.incompleteFrames) >= MAX_INCOMPLETE_FRAMES:
            oldestId = min(self.incompleteFrames, key=lambda candidate: serialDiff(candidate, frameId))
            if serialDiff(frameId, oldestId) < 0:
                return None
            self.discard([oldestId])
        partial = PartialFrame(fragment, nowNs)
        self.incompleteFrames[frameId] = partial
        return partial

    def deliver(self, frameId: int, partial: PartialFrame) -> Frame:
        del self.incompleteFrames[frameId]
        self.lastDeliveredId = frameId
        self.discard([candidate for candidate in self.incompleteFrames if serialDiff(candidate, frameId) < 0])
        self.stats.deliveredFrames += 1
        return Frame(frameId, partial.codec, partial.flags, partial.captureTimestampUs, bytes(partial.data))

    def discard(self, frameIds: list[int]) -> None:
        for frameId in frameIds:
            del self.incompleteFrames[frameId]
            self.stats.discardedFrames += 1
