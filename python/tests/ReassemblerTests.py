import random

import pytest

from freemonitor.Fragmenter import Fragmenter
from freemonitor.Packets import (
    FRAGMENT_TIMEOUT_MS,
    MAX_FRAME_SIZE,
    MAX_INCOMPLETE_FRAMES,
    Codec,
    FragmentFlags,
    PacketType,
    VideoFragment,
)
from freemonitor.Reassembler import Reassembler, serialDiff

TIMEOUT_NS = FRAGMENT_TIMEOUT_MS * 1_000_000
TWO_FRAGMENT_SIZE = 2000


def frameData(size, seed=0):
    return random.Random(seed).randbytes(size)


def fragmentsOf(data, frameId, sessionId=0, codec=Codec.JPEG, flags=FragmentFlags.KEYFRAME):
    return Fragmenter(sessionId=sessionId, clock=lambda: 0).fragmentFrame(data, frameId, codec, flags, 1234)


def pushAll(reassembler, packets, nowNs=0):
    return [frame for packet in packets if (frame := reassembler.push(packet, nowNs)) is not None]


def modified(packet, **changes):
    fragment = VideoFragment.unpack(packet)
    for name, value in changes.items():
        target = fragment.commonHeader if hasattr(fragment.commonHeader, name) else fragment
        setattr(target, name, value)
    return fragment.pack()


@pytest.mark.parametrize(
    ("newer", "older", "expected"),
    [
        (1, 0, 1),
        (0, 1, -1),
        (5, 5, 0),
        (0, 0xFFFFFFFF, 1),
        (0xFFFFFFFF, 0, -1),
        (2**31 - 1, 0, 2**31 - 1),
        (2**31, 0, -(2**31)),
    ],
)
def testSerialDiff(newer, older, expected):
    assert serialDiff(newer, older) == expected


def testSingleFragmentFrameIsDelivered():
    data = frameData(100)
    [frame] = pushAll(Reassembler(), fragmentsOf(data, 9))
    assert frame.frameId == 9
    assert frame.data == data
    assert frame.codec == Codec.JPEG
    assert frame.flags == FragmentFlags.KEYFRAME
    assert frame.captureTimestampUs == 1234


def testOutOfOrderFragmentsAreReassembled():
    data = frameData(20_000)
    packets = fragmentsOf(data, 1)
    random.Random(3).shuffle(packets)
    reassembler = Reassembler()
    [frame] = pushAll(reassembler, packets)
    assert frame.data == data
    assert reassembler.stats.deliveredFrames == 1


def testDuplicateFragmentIsIgnored():
    data = frameData(TWO_FRAGMENT_SIZE)
    first, second = fragmentsOf(data, 1)
    reassembler = Reassembler()
    assert pushAll(reassembler, [first, first]) == []
    assert reassembler.stats.duplicateFragments == 1
    [frame] = pushAll(reassembler, [second])
    assert frame.data == data


def testDuplicateDoesNotCountAsProgress():
    first, second = fragmentsOf(frameData(TWO_FRAGMENT_SIZE), 1)
    reassembler = Reassembler()
    reassembler.push(first, 0)
    reassembler.push(first, TIMEOUT_NS - 1)
    assert reassembler.push(second, TIMEOUT_NS) is None
    assert reassembler.stats.discardedFrames == 1


def testFrameWithMissingFragmentIsDiscardedAfterTimeout():
    packets = fragmentsOf(frameData(10_000), 1)
    reassembler = Reassembler()
    pushAll(reassembler, packets[:-1])
    reassembler.expire(TIMEOUT_NS - 1)
    assert len(reassembler.incompleteFrames) == 1
    reassembler.expire(TIMEOUT_NS)
    assert reassembler.incompleteFrames == {}
    assert reassembler.stats.discardedFrames == 1


def testSlowFrameThatKeepsProgressingIsKept():
    data = frameData(6000)
    packets = fragmentsOf(data, 1)
    reassembler = Reassembler()
    frames = [reassembler.push(packet, index * (TIMEOUT_NS - 1)) for index, packet in enumerate(packets)]
    assert frames[-1].data == data
    assert len(packets) * (TIMEOUT_NS - 1) > TIMEOUT_NS


def testOlderAndSameFramesAreDroppedAfterDelivery():
    reassembler = Reassembler()
    pushAll(reassembler, fragmentsOf(frameData(10), 10))
    assert pushAll(reassembler, fragmentsOf(frameData(10), 10)) == []
    assert pushAll(reassembler, fragmentsOf(frameData(10), 9)) == []
    assert reassembler.stats.staleFragments == 2
    assert len(pushAll(reassembler, fragmentsOf(frameData(10), 11))) == 1


def testDeliveringAFrameDiscardsOlderIncompleteFrames():
    olderFirst, olderSecond = fragmentsOf(frameData(TWO_FRAGMENT_SIZE), 1)
    reassembler = Reassembler()
    reassembler.push(olderFirst, 0)
    pushAll(reassembler, fragmentsOf(frameData(10), 2))
    assert reassembler.incompleteFrames == {}
    assert reassembler.stats.discardedFrames == 1
    assert reassembler.push(olderSecond, 0) is None
    assert reassembler.stats.staleFragments == 1


def testOldestIncompleteFrameIsEvictedWhenFull():
    reassembler = Reassembler()
    for frameId in range(1, MAX_INCOMPLETE_FRAMES + 2):
        reassembler.push(fragmentsOf(frameData(TWO_FRAGMENT_SIZE), frameId)[0], 0)
    assert sorted(reassembler.incompleteFrames) == list(range(2, MAX_INCOMPLETE_FRAMES + 2))
    assert reassembler.stats.discardedFrames == 1


def testFragmentOlderThanAllIncompleteFramesIsDropped():
    reassembler = Reassembler()
    for frameId in range(10, 10 + MAX_INCOMPLETE_FRAMES):
        reassembler.push(fragmentsOf(frameData(TWO_FRAGMENT_SIZE), frameId)[0], 0)
    reassembler.push(fragmentsOf(frameData(TWO_FRAGMENT_SIZE), 9)[0], 0)
    assert sorted(reassembler.incompleteFrames) == list(range(10, 10 + MAX_INCOMPLETE_FRAMES))
    assert reassembler.stats.staleFragments == 1
    assert reassembler.stats.discardedFrames == 0


def testFrameIdWrapsAround():
    reassembler = Reassembler()
    pushAll(reassembler, fragmentsOf(frameData(10), 0xFFFFFFFF))
    [frame] = pushAll(reassembler, fragmentsOf(frameData(10), 0))
    assert frame.frameId == 0


def testSessionIdMustMatch():
    reassembler = Reassembler(sessionId=42)
    assert pushAll(reassembler, fragmentsOf(frameData(10), 1, sessionId=0)) == []
    assert reassembler.stats.invalidPackets == 1
    assert len(pushAll(reassembler, fragmentsOf(frameData(10), 1, sessionId=42))) == 1


def testResetForgetsTheLastDeliveredFrame():
    reassembler = Reassembler()
    pushAll(reassembler, fragmentsOf(frameData(10), 10))
    reassembler.push(fragmentsOf(frameData(TWO_FRAGMENT_SIZE), 11)[0], 0)
    reassembler.reset()
    assert reassembler.incompleteFrames == {}
    assert len(pushAll(reassembler, fragmentsOf(frameData(10), 5))) == 1


INVALID_SECOND_FRAGMENTS = {
    "too short": lambda packet: packet[: VideoFragment.SIZE - 1],
    "magic": lambda packet: modified(packet, magic=0x1234),
    "version": lambda packet: modified(packet, version=2),
    "type": lambda packet: modified(packet, type=PacketType.CURSOR_POS),
    "session": lambda packet: modified(packet, sessionId=7),
    "header reserved": lambda packet: modified(packet, headerReserved=1),
    "fragment reserved": lambda packet: modified(packet, reserved=1),
    "reserved flag bit": lambda packet: modified(packet, flags=FragmentFlags.KEYFRAME | 0x02),
    "unknown codec": lambda packet: modified(packet, codec=2),
    "frame size zero": lambda packet: modified(packet, frameSize=0),
    "frame size too large": lambda packet: modified(packet, frameSize=MAX_FRAME_SIZE + 1),
    "fragment count mismatch": lambda packet: modified(packet, fragCount=3),
    "index past count": lambda packet: modified(packet, fragIndex=2),
    "payload too short": lambda packet: packet[:-1],
    "payload too long": lambda packet: packet + b"\x00",
    "different frame size": lambda packet: modified(packet, frameSize=TWO_FRAGMENT_SIZE + 1, payload=bytes(649)),
    "different codec": lambda packet: modified(packet, codec=Codec.H264),
    "different flags": lambda packet: modified(packet, flags=0),
    "different capture time": lambda packet: modified(packet, captureTimestampUs=999),
}


@pytest.mark.parametrize("corrupt", INVALID_SECOND_FRAGMENTS.values(), ids=INVALID_SECOND_FRAGMENTS.keys())
def testInvalidFragmentIsDroppedWithoutChangingState(corrupt):
    data = frameData(TWO_FRAGMENT_SIZE)
    first, second = fragmentsOf(data, 5)
    reassembler = Reassembler()
    reassembler.push(first, 0)
    assert reassembler.push(corrupt(second), TIMEOUT_NS - 1) is None
    assert reassembler.stats.invalidPackets == 1
    assert reassembler.incompleteFrames[5].receivedCount == 1
    assert reassembler.incompleteFrames[5].lastProgressNs == 0
    [frame] = pushAll(reassembler, [second], TIMEOUT_NS - 1)
    assert frame.data == data
