import io
import random
import select
import socket
import struct
import time

from PIL import Image

from freemonitor.Fragmenter import Fragmenter
from freemonitor.Packets import HOST_PORT, SESSION_TIMEOUT_MS, Codec, CursorPos, FragmentFlags
from tools.FrameViewer import (
    RECEIVE_BUFFER_SIZE,
    FrameReceiver,
    decodeJPEG,
    fitImage,
    parseArguments,
    receiveBufferWarning,
)
from tools.LossEmulator import Impairment, LossEmulator

LOCALHOST = "127.0.0.1"
QUIET_TIMEOUT_S = 0.5
FRAME_INTERVAL_S = 0.01
SESSION_TIMEOUT_NS = SESSION_TIMEOUT_MS * 1_000_000


def jpegBytes(width, height, imageFormat="JPEG"):
    output = io.BytesIO()
    Image.new("RGB", (width, height), (200, 30, 90)).save(output, imageFormat)
    return output.getvalue()


def receiveEveryFrame(receiver):
    frames = []
    while select.select([receiver.udpSocket], [], [], QUIET_TIMEOUT_S)[0]:
        frame = receiver.receivePending(maxPackets=1)
        if frame is not None:
            frames.append(frame)
    return frames


def sendFrames(frames, destination, fragmenter=None):
    fragmenter = fragmenter or Fragmenter()
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for frameId, data in frames.items():
            for packet in fragmenter.fragmentFrame(data, frameId, Codec.JPEG, FragmentFlags.KEYFRAME, frameId):
                sender.sendto(packet, destination)
            time.sleep(FRAME_INTERVAL_S)
    finally:
        sender.close()


def testFramesSurviveTheEmulatorIntoTheViewer():
    generator = random.Random(11)
    frames = {frameId: generator.randbytes(generator.randint(1, 20_000)) for frameId in range(1000, 1060)}
    impairment = Impairment(lossRate=0.05, jitterMs=2, reorderRate=0.05, reorderDelayMs=5, duplicateRate=0.02)
    receiver = FrameReceiver((LOCALHOST, 0))
    try:
        with LossEmulator((LOCALHOST, 0), receiver.address, impairment, seed=3) as emulator:
            emulator.start()
            sendFrames(frames, emulator.listenAddress)
            delivered = receiveEveryFrame(receiver)
    finally:
        receiver.close()
    assert len(delivered) >= 15
    assert all(frame.data == frames[frame.frameId] for frame in delivered)
    assert [frame.frameId for frame in delivered] == sorted(frame.frameId for frame in delivered)


def testJPEGFrameIsDecodedAfterTheEmulator():
    receiver = FrameReceiver((LOCALHOST, 0))
    try:
        with LossEmulator((LOCALHOST, 0), receiver.address, Impairment()) as emulator:
            emulator.start()
            sendFrames({1: jpegBytes(640, 480)}, emulator.listenAddress)
            [frame] = receiveEveryFrame(receiver)
    finally:
        receiver.close()
    assert decodeJPEG(frame.data).size == (640, 480)


def testDecodeJPEGRejectsOtherData():
    assert decodeJPEG(b"not a jpeg") is None
    assert decodeJPEG(jpegBytes(8, 8, "PNG")) is None
    assert decodeJPEG(jpegBytes(64, 64)[:100]) is None


def testDecodeJPEGRejectsHugeClaimedSize():
    data = bytearray(jpegBytes(8, 8))
    sizeOffset = data.index(b"\xff\xc0") + 5
    data[sizeOffset : sizeOffset + 4] = struct.pack("!HH", 60000, 60000)
    assert decodeJPEG(bytes(data)) is None


def testFitImageOnlyShrinks():
    assert fitImage(Image.new("RGB", (200, 100)), 100, 100).size == (100, 50)
    assert fitImage(Image.new("RGB", (50, 40)), 100, 100).size == (50, 40)


def testOtherPacketTypesAreCountedSeparately():
    receiver = FrameReceiver((LOCALHOST, 0))
    try:
        assert receiver.handleDatagram(CursorPos().pack()) is None
    finally:
        receiver.close()
    assert receiver.otherPackets == 1
    assert receiver.reassembler.stats.invalidPackets == 0


def testSenderRestartWithOlderFrameIdsIsDetected():
    now = [0]
    receiver = FrameReceiver((LOCALHOST, 0), clock=lambda: now[0])
    fragmenter = Fragmenter()
    try:
        [newPacket] = fragmenter.fragmentFrame(b"a", 1000, Codec.JPEG, FragmentFlags.KEYFRAME, 0)
        [restartedPacket] = fragmenter.fragmentFrame(b"b", 10, Codec.JPEG, FragmentFlags.KEYFRAME, 0)
        [laterPacket] = fragmenter.fragmentFrame(b"c", 11, Codec.JPEG, FragmentFlags.KEYFRAME, 0)
        assert receiver.handleDatagram(newPacket).frameId == 1000
        now[0] = SESSION_TIMEOUT_NS - 1
        assert receiver.handleDatagram(restartedPacket) is None
        now[0] = SESSION_TIMEOUT_NS
        assert receiver.handleDatagram(laterPacket).frameId == 11
    finally:
        receiver.close()
    assert receiver.senderRestarts == 1


def testPortDefaultsToHostPort():
    assert parseArguments([]).port == HOST_PORT
    assert parseArguments(["--port", "6000"]).port == 6000


def testReceiverRecordsTheBufferSizeTheKernelGranted():
    receiver = FrameReceiver((LOCALHOST, 0))
    try:
        assert receiver.receiveBufferSize == receiver.udpSocket.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
    finally:
        receiver.close()


def testReceiveBufferWarningOnlyForASmallerBuffer():
    assert receiveBufferWarning(RECEIVE_BUFFER_SIZE) is None
    assert receiveBufferWarning(2 * RECEIVE_BUFFER_SIZE) is None
    warning = receiveBufferWarning(425_984)
    assert "425984 bytes" in warning
    assert f"net.core.rmem_max={RECEIVE_BUFFER_SIZE}" in warning
