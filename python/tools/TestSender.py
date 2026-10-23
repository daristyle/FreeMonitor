import argparse
import io
import math
import random
import socket
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass

from PIL import Image, ImageDraw, ImageFont

from freemonitor.Fragmenter import SERIAL_MODULUS, Fragmenter, monotonicUs
from freemonitor.Packets import MAX_FRAME_SIZE, Codec, FragmentFlags
from tools.LossEmulator import parseAddress

SEND_BUFFER_SIZE = 8 * 1024 * 1024
REPORT_INTERVAL_NS = 1_000_000_000
MIN_DIMENSION = 16
MAX_DIMENSION = 65500
MIN_QUALITY = 1
MAX_QUALITY = 95
QUALITY_STEP = 10
BAR_COLORS = [
    (255, 255, 255),
    (255, 255, 0),
    (0, 255, 255),
    (0, 255, 0),
    (255, 0, 255),
    (255, 0, 0),
    (0, 0, 255),
    (0, 0, 0),
]
MOTION_BACKGROUND = (32, 32, 32)
BORDER_COLOR = (255, 255, 255)
MOVING_BAR_COLOR = (255, 255, 255)
TEXT_COLOR = (255, 255, 255)
TEXT_OUTLINE_COLOR = (0, 0, 0)


def encodeJPEG(image: Image.Image, quality: int, maxSize: int = MAX_FRAME_SIZE) -> tuple[bytes, int]:
    while True:
        output = io.BytesIO()
        image.save(output, "JPEG", quality=quality)
        data = output.getvalue()
        if len(data) <= maxSize:
            return data, quality
        if quality <= MIN_QUALITY:
            raise ValueError(f"JPEG is {len(data)} bytes at quality {quality}, more than {maxSize}")
        quality = max(MIN_QUALITY, quality - QUALITY_STEP)


def startValues(seed: int | None, firstFrameId: int | None) -> tuple[int, int]:
    generator = random.Random(seed)
    frameId = generator.getrandbits(32)
    seq = generator.getrandbits(32)
    if firstFrameId is not None:
        frameId = firstFrameId
    return frameId, seq


class TestPattern:
    __test__ = False

    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self.motionTop = height // 2
        self.barWidth = max(1, width // 64)
        self.barStep = max(1, width // 120)
        self.margin = max(1, height // 40)
        self.font = ImageFont.load_default(size=max(8, height // 10))
        self.outlineWidth = max(1, height // 240)
        self.background = self.drawBackground()

    def drawBackground(self) -> Image.Image:
        image = Image.new("RGB", (self.width, self.height), MOTION_BACKGROUND)
        draw = ImageDraw.Draw(image)
        for index, color in enumerate(BAR_COLORS):
            left = index * self.width // len(BAR_COLORS)
            right = (index + 1) * self.width // len(BAR_COLORS) - 1
            draw.rectangle((left, 0, right, self.motionTop - 1), fill=color)
        draw.rectangle((0, 0, self.width - 1, self.height - 1), outline=BORDER_COLOR)
        return image

    def render(self, frameId: int, timestampUs: int) -> Image.Image:
        image = self.background.copy()
        draw = ImageDraw.Draw(image)
        left = frameId * self.barStep % self.width
        draw.rectangle((left, self.motionTop, left + self.barWidth - 1, self.height - 2), fill=MOVING_BAR_COLOR)
        draw.text(
            (self.margin, self.motionTop + self.margin),
            f"frame {frameId}\nt = {timestampUs / 1_000_000:.3f} s",
            font=self.font,
            fill=TEXT_COLOR,
            stroke_width=self.outlineWidth,
            stroke_fill=TEXT_OUTLINE_COLOR,
        )
        return image


class FramePacer:
    def __init__(self, fps: float, startNs: int):
        self.intervalNs = max(1, round(1_000_000_000 / fps))
        self.nextFrameNs = startNs

    def waitNs(self, nowNs: int) -> int:
        return max(0, self.nextFrameNs - nowNs)

    def advance(self, nowNs: int) -> int:
        missed = max(0, (nowNs - self.nextFrameNs) // self.intervalNs)
        self.nextFrameNs += (missed + 1) * self.intervalNs
        return missed


@dataclass
class SenderStats:
    frames: int = 0
    packets: int = 0
    sentBytes: int = 0
    sendErrors: int = 0
    skippedFrames: int = 0
    lastFrameSize: int = 0
    lastQuality: int = 0

    def summary(self) -> str:
        return (
            f"{self.frames} frames, {self.packets} packets, {self.sentBytes / (1024 * 1024):.1f} MiB, "
            f"{self.sendErrors} send errors, {self.skippedFrames} skipped, "
            f"last frame {self.lastFrameSize / 1024:.1f} KiB at quality {self.lastQuality}"
        )


class TestSender:
    __test__ = False

    def __init__(
        self,
        targetAddress: tuple[str, int],
        width: int,
        height: int,
        quality: int,
        firstFrameId: int = 0,
        firstSeq: int = 0,
        clock: Callable[[], int] = monotonicUs,
        udpSocket: socket.socket | None = None,
    ):
        self.targetAddress = (socket.gethostbyname(targetAddress[0]), targetAddress[1])
        self.quality = quality
        self.frameId = firstFrameId % SERIAL_MODULUS
        self.clock = clock
        self.pattern = TestPattern(width, height)
        self.fragmenter = Fragmenter(sessionId=0, firstSeq=firstSeq, clock=clock)
        self.stats = SenderStats()
        if udpSocket is None:
            udpSocket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udpSocket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, SEND_BUFFER_SIZE)
        self.udpSocket = udpSocket

    def __enter__(self) -> TestSender:
        return self

    def __exit__(self, *exceptionInfo) -> None:
        self.close()

    def close(self) -> None:
        self.udpSocket.close()

    def sendFrame(self) -> None:
        captureTimestampUs = self.clock()
        image = self.pattern.render(self.frameId, captureTimestampUs)
        data, self.stats.lastQuality = encodeJPEG(image, self.quality)
        packets = self.fragmenter.fragmentFrame(
            data, self.frameId, Codec.JPEG, FragmentFlags.KEYFRAME, captureTimestampUs
        )
        for packet in packets:
            try:
                self.udpSocket.sendto(packet, self.targetAddress)
            except OSError:
                self.stats.sendErrors += 1
                continue
            self.stats.packets += 1
            self.stats.sentBytes += len(packet)
        self.stats.frames += 1
        self.stats.lastFrameSize = len(data)
        self.frameId = (self.frameId + 1) % SERIAL_MODULUS

    def run(self, fps: float, frameLimit: int = 0, report: Callable[[str], None] | None = None) -> None:
        pacer = FramePacer(fps, time.monotonic_ns())
        nextReportNs = time.monotonic_ns() + REPORT_INTERVAL_NS
        while frameLimit == 0 or self.stats.frames < frameLimit:
            nowNs = time.monotonic_ns()
            waitNs = pacer.waitNs(nowNs)
            if waitNs > 0:
                time.sleep(waitNs / 1e9)
                continue
            self.stats.skippedFrames += pacer.advance(nowNs)
            self.sendFrame()
            if report is not None and nowNs >= nextReportNs:
                report(self.stats.summary())
                nextReportNs += REPORT_INTERVAL_NS


def integerParser(minimum: int, maximum: int) -> Callable[[str], int]:
    def parse(text: str) -> int:
        try:
            value = int(text)
        except ValueError:
            raise argparse.ArgumentTypeError(f"expected an integer, got {text!r}") from None
        if not minimum <= value <= maximum:
            raise argparse.ArgumentTypeError(f"expected an integer from {minimum} to {maximum}, got {text!r}")
        return value

    return parse


def parseFps(text: str) -> float:
    try:
        fps = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected a frame rate, got {text!r}") from None
    if not math.isfinite(fps) or fps <= 0.0:
        raise argparse.ArgumentTypeError(f"expected a finite frame rate above 0, got {text!r}")
    return fps


def parseArguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Streams a JPEG test pattern in development mode (session ID 0), for the viewer and tablet."
    )
    parser.add_argument("--target", type=parseAddress, required=True, help="HOST:PORT to send the frames to")
    parser.add_argument("--width", type=integerParser(MIN_DIMENSION, MAX_DIMENSION), default=1280)
    parser.add_argument("--height", type=integerParser(MIN_DIMENSION, MAX_DIMENSION), default=720)
    parser.add_argument("--fps", type=parseFps, default=30.0, help="frames per second")
    parser.add_argument("--quality", type=integerParser(MIN_QUALITY, MAX_QUALITY), default=80, help="JPEG quality")
    parser.add_argument(
        "--frames", type=integerParser(0, sys.maxsize), default=0, help="frames to send, 0 sends until stopped"
    )
    parser.add_argument("--seed", type=int, default=None, help="random seed for the first frameId and seq")
    parser.add_argument(
        "--first-frame-id", type=integerParser(0, SERIAL_MODULUS - 1), default=None, help="first frameId to send"
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parseArguments(argv)
    firstFrameId, firstSeq = startValues(arguments.seed, arguments.first_frame_id)
    with TestSender(
        arguments.target, arguments.width, arguments.height, arguments.quality, firstFrameId, firstSeq
    ) as sender:
        host, port = sender.targetAddress
        print(
            f"Sending {arguments.width}x{arguments.height} JPEG at {arguments.fps:g} fps to {host}:{port}, "
            f"first frameId {firstFrameId}, first seq {firstSeq}"
        )
        try:
            sender.run(arguments.fps, arguments.frames, lambda text: print(text, flush=True))
        except KeyboardInterrupt:
            pass
        print(sender.stats.summary())
    return 0


if __name__ == "__main__":
    sys.exit(main())
