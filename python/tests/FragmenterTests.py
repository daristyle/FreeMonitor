import itertools

import pytest

from freemonitor.Fragmenter import Fragmenter, fragmentCount
from freemonitor.Packets import (
    MAX_FRAGMENT_PAYLOAD,
    MAX_FRAME_SIZE,
    MAX_PACKET_SIZE,
    Codec,
    FragmentFlags,
    PacketType,
    VideoFragment,
)


def fragmentFrame(frameData, fragmenter=None):
    fragmenter = fragmenter or Fragmenter(sessionId=0, clock=lambda: 0)
    return [
        VideoFragment.unpack(packet)
        for packet in fragmenter.fragmentFrame(frameData, 7, Codec.JPEG, FragmentFlags.KEYFRAME, 1234)
    ]


def testSpecExampleOf50000Bytes():
    frameData = bytes(range(256)) * 195 + bytes(80)
    fragments = fragmentFrame(frameData)
    assert len(fragments) == 37
    assert [len(fragment.payload) for fragment in fragments] == [1352] * 36 + [1328]
    assert b"".join(fragment.payload for fragment in fragments) == frameData


def testFragmentHeadersFollowTheSpec():
    for fragIndex, fragment in enumerate(fragmentFrame(bytes(3000))):
        assert fragment.commonHeader.type == PacketType.VIDEO_FRAGMENT
        assert fragment.frameId == 7
        assert fragment.fragIndex == fragIndex
        assert fragment.fragCount == 3
        assert fragment.frameSize == 3000
        assert fragment.codec == Codec.JPEG
        assert fragment.flags == FragmentFlags.KEYFRAME
        assert fragment.captureTimestampUs == 1234


def testExactMultipleHasOnlyFullFragments():
    fragments = fragmentFrame(bytes(2 * MAX_FRAGMENT_PAYLOAD))
    assert [len(fragment.payload) for fragment in fragments] == [MAX_FRAGMENT_PAYLOAD] * 2


def testPacketsFitInMaxPacketSize():
    fragmenter = Fragmenter()
    packets = fragmenter.fragmentFrame(bytes(10_000), 1, Codec.JPEG, FragmentFlags.KEYFRAME, 0)
    assert max(len(packet) for packet in packets) == MAX_PACKET_SIZE


def testSeqIncreasesPerPacketAndWraps():
    fragmenter = Fragmenter(firstSeq=0xFFFFFFFE, clock=lambda: 0)
    first = fragmentFrame(bytes(3 * MAX_FRAGMENT_PAYLOAD), fragmenter)
    second = fragmentFrame(bytes(1), fragmenter)
    assert [fragment.commonHeader.seq for fragment in first + second] == [0xFFFFFFFE, 0xFFFFFFFF, 0, 1]


def testTimestampIsReadPerPacket():
    ticks = itertools.count(100)
    fragments = fragmentFrame(bytes(3000), Fragmenter(clock=lambda: next(ticks)))
    assert [fragment.commonHeader.timestampUs for fragment in fragments] == [100, 101, 102]


def testSessionIdIsWritten():
    fragments = fragmentFrame(bytes(10), Fragmenter(sessionId=42))
    assert fragments[0].commonHeader.sessionId == 42


@pytest.mark.parametrize("frameSize", [0, MAX_FRAME_SIZE + 1])
def testFrameSizeOutsideLimitsIsRejected(frameSize):
    with pytest.raises(ValueError, match="outside"):
        Fragmenter().fragmentFrame(bytes(frameSize), 1, Codec.JPEG, FragmentFlags.KEYFRAME, 0)


def testLargestFrameIsAccepted():
    packets = Fragmenter().fragmentFrame(bytes(MAX_FRAME_SIZE), 1, Codec.JPEG, FragmentFlags.KEYFRAME, 0)
    assert len(packets) == fragmentCount(MAX_FRAME_SIZE) == 3103
