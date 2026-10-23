import argparse
import io
import random
import select
import socket

import pytest
from PIL import Image

from freemonitor.Fragmenter import SERIAL_MODULUS
from freemonitor.Packets import MAX_PACKET_SIZE, Codec, FragmentFlags, PacketType, VideoFragment
from tools.FrameViewer import FrameReceiver, decodeJPEG
from tools.LossEmulator import Impairment, LossEmulator
from tools.TestSender import (
    FramePacer,
    TestPattern,
    TestSender,
    encodeJPEG,
    integerParser,
    main,
    parseArguments,
    parseFps,
    startValues,
)

LOCALHOST = "127.0.0.1"
QUIET_TIMEOUT_S = 0.5
WIDTH = 320
HEIGHT = 180


class FailingSocket:
    def sendto(self, data, address):
        raise OSError("network is unreachable")

    def close(self):
        pass


def openReceiver():
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    receiver.bind((LOCALHOST, 0))
    return receiver


def receiveAll(receiver):
    packets = []
    while select.select([receiver], [], [], QUIET_TIMEOUT_S)[0]:
        packets.append(receiver.recv(65535))
    return packets


def receiveEveryFrame(receiver):
    frames = []
    while select.select([receiver.udpSocket], [], [], QUIET_TIMEOUT_S)[0]:
        frame = receiver.receivePending(maxPackets=1)
        if frame is not None:
            frames.append(frame)
    return frames


def testPatternIsABaselineJFIFOfTheRequestedSize():
    data, quality = encodeJPEG(TestPattern(WIDTH, HEIGHT).render(7, 123_456), 80)
    image = Image.open(io.BytesIO(data))
    assert quality == 80
    assert image.format == "JPEG"
    assert image.size == (WIDTH, HEIGHT)
    assert image.mode == "RGB"
    assert "jfif" in image.info
    assert "progressive" not in image.info


def testConsecutiveFramesLookDifferent():
    pattern = TestPattern(WIDTH, HEIGHT)
    assert pattern.render(1, 0).tobytes() != pattern.render(2, 0).tobytes()


def testRenderingDoesNotChangeTheBackground():
    pattern = TestPattern(WIDTH, HEIGHT)
    before = pattern.background.tobytes()
    pattern.render(3, 0)
    assert pattern.background.tobytes() == before


def testPatternRendersAtTheSmallestSize():
    image = TestPattern(16, 16).render(SERIAL_MODULUS - 1, 0)
    assert image.size == (16, 16)


def testQualityIsLoweredToFitTheMaximumSize():
    generator = random.Random(5)
    noise = Image.frombytes("RGB", (256, 256), generator.randbytes(256 * 256 * 3))
    fullSize = len(encodeJPEG(noise, 95)[0])
    data, quality = encodeJPEG(noise, 95, maxSize=fullSize // 2)
    assert quality < 95
    assert len(data) <= fullSize // 2
    assert decodeJPEG(data) is not None


def testEncodingFailsWhenEvenTheLowestQualityIsTooLarge():
    with pytest.raises(ValueError):
        encodeJPEG(TestPattern(WIDTH, HEIGHT).render(0, 0), 80, maxSize=100)


def testStartValuesAreRepeatableWithASeed():
    assert startValues(9, None) == startValues(9, None)
    assert startValues(9, None) != startValues(10, None)
    frameId, seq = startValues(9, None)
    assert 0 <= frameId < SERIAL_MODULUS
    assert 0 <= seq < SERIAL_MODULUS


def testFirstFrameIdOverridesTheRandomValue():
    assert startValues(9, 1234) == (1234, startValues(9, None)[1])


def testPacerSendsOnTimeFramesWithoutSkipping():
    pacer = FramePacer(100, 0)
    assert pacer.waitNs(0) == 0
    assert pacer.advance(0) == 0
    assert pacer.waitNs(4_000_000) == 6_000_000
    assert pacer.advance(10_000_000) == 0
    assert pacer.waitNs(10_000_000) == 10_000_000


def testPacerSkipsMissedSlotsInsteadOfCatchingUp():
    pacer = FramePacer(100, 0)
    pacer.advance(0)
    assert pacer.advance(35_000_000) == 2
    assert pacer.waitNs(35_000_000) == 5_000_000


def testPacketsFollowDevelopmentMode():
    timestamps = iter(range(1_000, 100_000, 10))
    receiver = openReceiver()
    try:
        with TestSender(
            receiver.getsockname(),
            WIDTH,
            HEIGHT,
            80,
            firstFrameId=SERIAL_MODULUS - 2,
            firstSeq=SERIAL_MODULUS - 1,
            clock=lambda: next(timestamps),
        ) as sender:
            for _ in range(3):
                sender.sendFrame()
            stats = sender.stats
        packets = receiveAll(receiver)
    finally:
        receiver.close()
    fragments = [VideoFragment.unpack(packet) for packet in packets]
    assert len(fragments) == stats.packets
    assert stats.frames == 3
    assert stats.sendErrors == 0
    assert all(len(packet) <= MAX_PACKET_SIZE for packet in packets)
    assert [fragment.commonHeader.seq for fragment in fragments] == [
        (SERIAL_MODULUS - 1 + index) % SERIAL_MODULUS for index in range(len(fragments))
    ]
    assert sorted({fragment.frameId for fragment in fragments}) == [0, SERIAL_MODULUS - 2, SERIAL_MODULUS - 1]
    for fragment in fragments:
        assert fragment.commonHeader.type == PacketType.VIDEO_FRAGMENT
        assert fragment.commonHeader.sessionId == 0
        assert fragment.codec == Codec.JPEG
        assert fragment.flags == FragmentFlags.KEYFRAME
    firstFrame = [fragment for fragment in fragments if fragment.frameId == SERIAL_MODULUS - 2]
    assert {fragment.captureTimestampUs for fragment in firstFrame} == {1_000}
    image = decodeJPEG(b"".join(fragment.payload for fragment in firstFrame))
    assert image is not None
    assert image.size == (WIDTH, HEIGHT)


def testSendErrorsAreCountedNotRaised():
    with TestSender((LOCALHOST, 9), WIDTH, HEIGHT, 80, udpSocket=FailingSocket()) as sender:
        sender.sendFrame()
        sender.sendFrame()
    assert sender.stats.frames == 2
    assert sender.stats.packets == 0
    assert sender.stats.sendErrors > 0


def testTestPatternSurvivesTheEmulatorIntoTheViewer():
    impairment = Impairment(lossRate=0.05, jitterMs=2, reorderRate=0.05, reorderDelayMs=5, duplicateRate=0.02)
    receiver = FrameReceiver((LOCALHOST, 0))
    try:
        with LossEmulator((LOCALHOST, 0), receiver.address, impairment, seed=3) as emulator:
            emulator.start()
            firstFrameId, firstSeq = startValues(4, None)
            with TestSender(emulator.listenAddress, WIDTH, HEIGHT, 80, firstFrameId, firstSeq) as sender:
                sender.run(fps=100, frameLimit=60)
            delivered = receiveEveryFrame(receiver)
    finally:
        receiver.close()
    assert len(delivered) >= 15
    for frame in delivered:
        assert frame.codec == Codec.JPEG
        image = decodeJPEG(frame.data)
        assert image is not None
        assert image.size == (WIDTH, HEIGHT)


def testMainSendsTheRequestedNumberOfFrames(capsys):
    receiver = FrameReceiver((LOCALHOST, 0))
    try:
        host, port = receiver.address
        exitCode = main(
            ["--target", f"{host}:{port}", "--width", "64", "--height", "48", "--fps", "200", "--frames", "5"]
        )
        delivered = receiveEveryFrame(receiver)
    finally:
        receiver.close()
    assert exitCode == 0
    assert len(delivered) == 5
    assert "5 frames" in capsys.readouterr().out


def testArgumentDefaults():
    arguments = parseArguments(["--target", "127.0.0.1:50500"])
    assert arguments.target == ("127.0.0.1", 50500)
    assert (arguments.width, arguments.height) == (1280, 720)
    assert arguments.fps == 30.0
    assert arguments.quality == 80
    assert arguments.frames == 0
    assert arguments.seed is None
    assert arguments.first_frame_id is None


@pytest.mark.parametrize("text", ["15", "65501", "abc", "1.5"])
def testInvalidDimensionsAreRejected(text):
    with pytest.raises(argparse.ArgumentTypeError):
        integerParser(16, 65500)(text)


@pytest.mark.parametrize("text", ["0", "-5", "nan", "inf", "fast"])
def testInvalidFrameRatesAreRejected(text):
    with pytest.raises(argparse.ArgumentTypeError):
        parseFps(text)


def testFrameIdAboveTheSerialRangeIsRejected():
    with pytest.raises(SystemExit):
        parseArguments(["--target", "127.0.0.1:50500", "--first-frame-id", str(SERIAL_MODULUS)])
