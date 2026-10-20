import time
from collections.abc import Callable

from freemonitor.Packets import MAX_FRAGMENT_PAYLOAD, MAX_FRAME_SIZE, CommonHeader, PacketType, VideoFragment

SERIAL_MODULUS = 2**32


def fragmentCount(frameSize: int) -> int:
    return -(-frameSize // MAX_FRAGMENT_PAYLOAD)


def fragmentPayloadSize(frameSize: int, fragIndex: int) -> int:
    return min(MAX_FRAGMENT_PAYLOAD, frameSize - fragIndex * MAX_FRAGMENT_PAYLOAD)


def monotonicUs() -> int:
    return time.monotonic_ns() // 1000


class Fragmenter:
    def __init__(self, sessionId: int = 0, firstSeq: int = 0, clock: Callable[[], int] = monotonicUs):
        self.sessionId = sessionId
        self.nextSeq = firstSeq % SERIAL_MODULUS
        self.clock = clock

    def fragmentFrame(
        self, frameData: bytes, frameId: int, codec: int, flags: int, captureTimestampUs: int
    ) -> list[bytes]:
        frameSize = len(frameData)
        if not 1 <= frameSize <= MAX_FRAME_SIZE:
            raise ValueError(f"frame size {frameSize} is outside 1..{MAX_FRAME_SIZE}")
        count = fragmentCount(frameSize)
        packets = []
        for fragIndex in range(count):
            offset = fragIndex * MAX_FRAGMENT_PAYLOAD
            header = CommonHeader(
                type=PacketType.VIDEO_FRAGMENT, sessionId=self.sessionId, seq=self.nextSeq, timestampUs=self.clock()
            )
            fragment = VideoFragment(
                commonHeader=header,
                frameId=frameId,
                fragIndex=fragIndex,
                fragCount=count,
                frameSize=frameSize,
                codec=codec,
                flags=flags,
                captureTimestampUs=captureTimestampUs,
                payload=frameData[offset : offset + MAX_FRAGMENT_PAYLOAD],
            )
            packets.append(fragment.pack())
            self.nextSeq = (self.nextSeq + 1) % SERIAL_MODULUS
        return packets
