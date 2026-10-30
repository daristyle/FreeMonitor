import os
import random
import subprocess
from dataclasses import dataclass, replace
from itertools import chain

import pytest

from freemonitor.Fragmenter import SERIAL_MODULUS, Fragmenter
from freemonitor.Packets import (
    ACK,
    FRAGMENT_TIMEOUT_MS,
    MAX_FRAGMENT_PAYLOAD,
    MAX_FRAME_SIZE,
    MAX_INCOMPLETE_FRAMES,
    Bye,
    Codec,
    CodecFlags,
    CursorFlags,
    CursorPos,
    CursorShape,
    FeatureFlags,
    FragmentFlags,
    Heartbeat,
    Hello,
    KeyframeRequest,
    Reject,
    RejectReason,
    ShapeRequest,
    Stats,
    VideoFragment,
    Welcome,
)
from freemonitor.Reassembler import Reassembler

SESSION_ID = 0x01020304
CAPTURE_TIMESTAMP_US = 0x1122334455667788
TIMESTAMP_US = 0x0102030405060708
NS_PER_MS = 1_000_000
FRAME_SIZES = [1, MAX_FRAGMENT_PAYLOAD - 1, MAX_FRAGMENT_PAYLOAD, MAX_FRAGMENT_PAYLOAD + 1, 50_000, MAX_FRAME_SIZE]
SHORT_FRAME_SIZE = 3 * MAX_FRAGMENT_PAYLOAD
SHUFFLE_SEEDS = range(5)
BAD_FIELDS = {
    "magic": (0, "0000"),
    "version": (2, "ff"),
    "type": (3, "02"),
    "sessionId": (4, "00000000"),
    "headerReserved": (12, "00000001"),
    "fragIndex": (28, "0003"),
    "fragCount zero": (30, "0000"),
    "fragCount wrong": (30, "0004"),
    "frameSize zero": (32, "00000000"),
    "frameSize too large": (32, (MAX_FRAME_SIZE + 1).to_bytes(4).hex()),
    "codec": (36, "02"),
    "flags": (37, "02"),
    "reserved": (38, "0001"),
}
BAD_LENGTHS = {
    "shorter than the common header": lambda packet: packet[:23],
    "shorter than the fragment header": lambda packet: packet[:47],
    "payload too short": lambda packet: packet[:-1],
    "payload too long": lambda packet: packet + b"\x00",
}


@dataclass(frozen=True)
class FrameResult:
    frameId: int
    codec: int
    keyframe: bool
    captureTimestampUs: int
    data: bytes


@dataclass(frozen=True)
class ReassemblyResult:
    statuses: list[str | FrameResult]
    framesCompleted: int
    framesDropped: int


class ConformanceTool:
    def __init__(self, path: str):
        self.path = path

    def run(self, arguments: list[str], lines: list[str]) -> list[str]:
        completed = subprocess.run(
            [self.path, *arguments],
            input="".join(f"{line}\n" for line in lines),
            capture_output=True,
            text=True,
            check=True,
        )
        return completed.stdout.splitlines()

    def fragment(self, frameData: bytes, frameId: int, codec: int, keyframe: bool, firstSeq: int) -> list[bytes] | None:
        arguments = ["fragment", SESSION_ID, firstSeq, frameId, int(codec), int(keyframe)]
        arguments += [CAPTURE_TIMESTAMP_US, TIMESTAMP_US]
        lines = self.run([str(argument) for argument in arguments], [frameData.hex()])
        return None if lines == ["INVALID"] else [bytes.fromhex(line) for line in lines]

    def reassemble(self, timedPackets: list[tuple[int, bytes]]) -> ReassemblyResult:
        lines = [f"{nowMs} {packet.hex()}" for nowMs, packet in timedPackets]
        *statusLines, totalLine = self.run(["reassemble", str(SESSION_ID)], lines)
        _, framesCompleted, framesDropped = totalLine.split()
        return ReassemblyResult(
            [self.parseStatus(line) for line in statusLines], int(framesCompleted), int(framesDropped)
        )

    def parseStatus(self, line: str) -> str | FrameResult:
        fields = line.split()
        if fields[0] == "FRAME_COMPLETE":
            frameId, codec, keyframe, captureTimestampUs, data = fields[1:]
            return FrameResult(int(frameId), int(codec), keyframe == "1", int(captureTimestampUs), bytes.fromhex(data))
        return "INVALID" if fields[0] == "IGNORED" else fields[0]

    def packet(self, packet: bytes) -> bytes | None:
        lines = self.run(["packet"], [packet.hex()])
        return None if lines == ["INVALID"] else bytes.fromhex(lines[0])


@pytest.fixture(scope="module")
def tool():
    path = os.environ.get("FM_CONFORMANCE_TOOL")
    if path is None:
        pytest.skip("FM_CONFORMANCE_TOOL is not set")
    return ConformanceTool(path)


def frameBytes(size: int) -> bytes:
    return random.Random(size).randbytes(size)


def makePackets(
    frameId: int,
    size: int = SHORT_FRAME_SIZE,
    codec: int = Codec.H264,
    flags: int = 0,
    captureTimestampUs: int = CAPTURE_TIMESTAMP_US,
    firstSeq: int = 0,
) -> list[bytes]:
    fragmenter = Fragmenter(sessionId=SESSION_ID, firstSeq=firstSeq, clock=lambda: TIMESTAMP_US)
    return fragmenter.fragmentFrame(frameBytes(size), frameId, codec, flags, captureTimestampUs)


def timed(packets: list[bytes]) -> list[tuple[int, bytes]]:
    return list(enumerate(packets))


def editPacket(packet: bytes, offset: int, hexValue: str) -> bytes:
    value = bytes.fromhex(hexValue)
    return packet[:offset] + value + packet[offset + len(value) :]


def pythonStatus(reassembler: Reassembler, packet: bytes, nowNs: int) -> str | FrameResult:
    before = replace(reassembler.stats)
    frame = reassembler.push(packet, nowNs)
    stats = reassembler.stats
    if frame is not None:
        keyframe = bool(frame.flags & FragmentFlags.KEYFRAME)
        return FrameResult(frame.frameId, frame.codec, keyframe, frame.captureTimestampUs, frame.data)
    if stats.invalidPackets != before.invalidPackets:
        return "INVALID"
    if stats.duplicateFragments != before.duplicateFragments:
        return "DUPLICATE"
    if stats.staleFragments != before.staleFragments:
        return "TOO_OLD"
    return "ACCEPTED"


def pythonReassemble(timedPackets: list[tuple[int, bytes]]) -> ReassemblyResult:
    reassembler = Reassembler(SESSION_ID)
    statuses = [pythonStatus(reassembler, packet, nowMs * NS_PER_MS) for nowMs, packet in timedPackets]
    reassembler.expire(timedPackets[-1][0] * NS_PER_MS)
    return ReassemblyResult(statuses, reassembler.stats.deliveredFrames, reassembler.stats.discardedFrames)


def reassemblyScenarios() -> dict[str, list[tuple[int, bytes]]]:
    first = makePackets(10)
    second = makePackets(11)
    wrapped = makePackets(SERIAL_MODULUS - 1)
    zero = makePackets(0)
    conflicting = makePackets(10, captureTimestampUs=1)
    timeout = FRAGMENT_TIMEOUT_MS
    incomplete = [makePackets(20 + index)[0] for index in range(MAX_INCOMPLETE_FRAMES)]
    return {
        "in order": timed(first),
        "reversed": timed(first[::-1]),
        "duplicates": timed([first[0], first[0], first[1], first[2], first[2]]),
        "interleaved frames": timed(list(chain.from_iterable(zip(first, second, strict=True)))),
        "newer frame discards an older one": timed(first[:2] + second),
        "late fragment of a replaced frame": timed(first[:2] + second + first[2:]),
        "delivered frame arrives again": timed(first + first[:1]),
        "older frame after a newer one": timed(second + first),
        "frameId wraps": timed(wrapped + zero),
        "frame from before the wrap": timed(zero + wrapped),
        "too many incomplete frames": timed(incomplete + [makePackets(24)[0]]),
        "fragment older than all incomplete frames": timed(incomplete + [makePackets(19)[0]]),
        "conflicting fragment": timed([first[0], conflicting[1], first[1], first[2]]),
        "timeout restarts a frame": [(0, first[0]), (timeout, first[1]), (timeout + 1, first[2])],
        "last moment before the timeout": [(0, first[0]), (timeout - 1, first[1]), (timeout - 1, first[2])],
        "progress restarts the timeout": [(0, first[0]), (timeout - 1, first[1]), (2 * timeout - 2, first[2])],
        "duplicate does not restart the timeout": [
            (0, first[0]),
            (timeout - 1, first[0]),
            (timeout, first[1]),
            (timeout + 1, first[2]),
        ],
        "conflicting fragment after the timeout": [(0, first[0]), (timeout, conflicting[1])],
    }


SCENARIOS = reassemblyScenarios()


def samplePackets() -> dict[str, object]:
    packets = [
        VideoFragment(
            frameId=0xFFFFFFFF,
            fragCount=1,
            frameSize=4,
            codec=Codec.H264,
            flags=FragmentFlags.KEYFRAME,
            captureTimestampUs=CAPTURE_TIMESTAMP_US,
            payload=b"\x00\x01\xfe\xff",
        ),
        CursorPos(shapeId=7, x=-20, y=-1, flags=CursorFlags.VISIBLE),
        CursorShape(shapeId=7, fragCount=1, width=32, height=48, hotspotX=3, hotspotY=4, payload=b"\x00\x01\xfe\xff"),
        Hello(width=2944, height=1840, refreshHz=60, codecs=CodecFlags.JPEG, features=FeatureFlags.CURSOR_OVERLAY),
        Welcome(width=2944, height=1840, fps=60, codec=Codec.H264, features=FeatureFlags.CURSOR_OVERLAY),
        ACK(ackedMsgId=0xFFFFFFFE),
        Heartbeat(echoTimestampUs=2**64 - 1, echoDelayUs=0xFFFFFFFF),
        KeyframeRequest(),
        ShapeRequest(shapeId=7),
        Stats(
            packetsReceived=1,
            packetsLost=2,
            framesCompleted=3,
            framesDropped=4,
            bytesReceived=0xFFFFFFFF,
        ),
        Bye(),
        Reject(reason=RejectReason.UNSUPPORTED_VERSION, version=2),
    ]
    for packet in packets:
        packet.commonHeader.sessionId = SESSION_ID
        packet.commonHeader.seq = 0xFFFFFFFF
        packet.commonHeader.timestampUs = TIMESTAMP_US
        if hasattr(packet, "controlHeader"):
            packet.controlHeader.msgId = 0xFFFFFFFD
    return {type(packet).__name__: packet for packet in packets}


SAMPLE_PACKETS = samplePackets()


@pytest.mark.parametrize("frameSize", FRAME_SIZES)
def testFragmentsMatchCPPByteForByte(tool, frameSize):
    expected = makePackets(1234, frameSize)
    assert tool.fragment(frameBytes(frameSize), 1234, Codec.H264, False, 0) == expected


@pytest.mark.parametrize(("codec", "keyframe"), [(Codec.JPEG, True), (Codec.H264, True), (Codec.H264, False)])
def testCodecAndKeyframeMatchCPP(tool, codec, keyframe):
    flags = FragmentFlags.KEYFRAME if keyframe else 0
    expected = makePackets(1234, codec=codec, flags=flags)
    assert tool.fragment(frameBytes(SHORT_FRAME_SIZE), 1234, codec, keyframe, 0) == expected


def testSeqWrapMatchesCPP(tool):
    firstSeq = SERIAL_MODULUS - 1
    expected = makePackets(1234, firstSeq=firstSeq)
    assert tool.fragment(frameBytes(SHORT_FRAME_SIZE), 1234, Codec.H264, False, firstSeq) == expected


@pytest.mark.parametrize("frameSize", [0, MAX_FRAME_SIZE + 1])
def testFrameSizeOutOfRangeIsRejectedByBoth(tool, frameSize):
    assert tool.fragment(bytes(frameSize), 1234, Codec.H264, False, 0) is None
    with pytest.raises(ValueError):
        makePackets(1234, frameSize)


@pytest.mark.parametrize("frameSize", FRAME_SIZES)
def testCPPRebuildsPythonFrames(tool, frameSize):
    packets = makePackets(1234, frameSize, Codec.JPEG, FragmentFlags.KEYFRAME)
    expected = FrameResult(1234, Codec.JPEG, True, CAPTURE_TIMESTAMP_US, frameBytes(frameSize))
    assert tool.reassemble(timed(packets)).statuses[-1] == expected


@pytest.mark.parametrize("frameSize", FRAME_SIZES)
def testPythonRebuildsCPPFrames(tool, frameSize):
    packets = tool.fragment(frameBytes(frameSize), 1234, Codec.JPEG, True, 0)
    expected = FrameResult(1234, Codec.JPEG, True, CAPTURE_TIMESTAMP_US, frameBytes(frameSize))
    assert pythonReassemble(timed(packets)).statuses[-1] == expected


@pytest.mark.parametrize("scenario", SCENARIOS)
def testReassemblersAgree(tool, scenario):
    timedPackets = SCENARIOS[scenario]
    assert tool.reassemble(timedPackets) == pythonReassemble(timedPackets)


@pytest.mark.parametrize("seed", SHUFFLE_SEEDS)
def testReassemblersAgreeOnShuffledFragments(tool, seed):
    packets = makePackets(10) + makePackets(11) + makePackets(12)
    random.Random(seed).shuffle(packets)
    assert tool.reassemble(timed(packets)) == pythonReassemble(timed(packets))


def assertInvalidPacketIsDropped(tool, badPacket):
    first, second, third = makePackets(10)
    timedPackets = timed([badPacket, first, badPacket, second, third])
    result = tool.reassemble(timedPackets)
    assert result == pythonReassemble(timedPackets)
    assert result.statuses[0] == "INVALID"
    assert result.statuses[2] == "INVALID"
    assert isinstance(result.statuses[4], FrameResult)


@pytest.mark.parametrize("field", BAD_FIELDS)
def testBadFieldIsDroppedByBoth(tool, field):
    offset, hexValue = BAD_FIELDS[field]
    assertInvalidPacketIsDropped(tool, editPacket(makePackets(10)[1], offset, hexValue))


@pytest.mark.parametrize("length", BAD_LENGTHS)
def testBadLengthIsDroppedByBoth(tool, length):
    assertInvalidPacketIsDropped(tool, BAD_LENGTHS[length](makePackets(10)[1]))


@pytest.mark.parametrize("name", SAMPLE_PACKETS)
def testCPPWritesBackEveryPacketUnchanged(tool, name):
    packet = SAMPLE_PACKETS[name].pack()
    assert tool.packet(packet) == packet


@pytest.mark.parametrize("name", SAMPLE_PACKETS)
def testShortPacketIsRejectedByBoth(tool, name):
    sample = SAMPLE_PACKETS[name]
    shortPacket = sample.pack()[: sample.SIZE - 1]
    assert tool.packet(shortPacket) is None
    with pytest.raises(ValueError):
        type(sample).unpack(shortPacket)
