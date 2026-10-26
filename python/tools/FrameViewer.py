import argparse
import io
import socket
import sys
import time
import tkinter
from collections.abc import Callable

from PIL import Image, ImageTk

from freemonitor.Packets import HOST_PORT, MAGIC, SESSION_TIMEOUT_MS, Codec, CommonHeader, FragmentFlags, PacketType
from freemonitor.Reassembler import Frame, Reassembler

RECEIVE_BUFFER_SIZE = 8 * 1024 * 1024
MAX_DATAGRAM_SIZE = 65535
MAX_PACKETS_PER_POLL = 20_000
POLL_INTERVAL_MS = 1
STATUS_INTERVAL_NS = 1_000_000_000
SESSION_TIMEOUT_NS = SESSION_TIMEOUT_MS * 1_000_000
DEVELOPMENT_SESSION_ID = 0
INITIAL_WINDOW_SIZE = "1280x720"


def decodeJPEG(data: bytes) -> Image.Image | None:
    try:
        image = Image.open(io.BytesIO(data))
        if image.format != "JPEG":
            return None
        image.load()
    except OSError, SyntaxError, ValueError, Image.DecompressionBombError:
        return None
    return image


def fitImage(image: Image.Image, width: int, height: int) -> Image.Image:
    scale = min(width / image.width, height / image.height, 1.0)
    if scale >= 1.0:
        return image
    size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    return image.resize(size, Image.Resampling.BILINEAR)


def receiveBufferWarning(receiveBufferSize: int) -> str | None:
    if receiveBufferSize >= RECEIVE_BUFFER_SIZE:
        return None
    return (
        f"Warning: the receive buffer is {receiveBufferSize} bytes, less than the {RECEIVE_BUFFER_SIZE} requested. "
        "Frames that arrive as one burst may be dropped. "
        f"Raise net.core.rmem_max: sudo sysctl -w net.core.rmem_max={RECEIVE_BUFFER_SIZE}"
    )


class FrameReceiver:
    def __init__(self, bindAddress: tuple[str, int], clock: Callable[[], int] = time.monotonic_ns):
        self.udpSocket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udpSocket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RECEIVE_BUFFER_SIZE)
        self.receiveBufferSize = self.udpSocket.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        self.udpSocket.bind(bindAddress)
        self.udpSocket.setblocking(False)
        self.clock = clock
        self.reassembler = Reassembler(DEVELOPMENT_SESSION_ID)
        self.lastDeliveryNs = clock()
        self.otherPackets = 0
        self.senderRestarts = 0

    @property
    def address(self) -> tuple[str, int]:
        return self.udpSocket.getsockname()

    def close(self) -> None:
        self.udpSocket.close()

    def receivePending(self, maxPackets: int = MAX_PACKETS_PER_POLL) -> Frame | None:
        newest = None
        for _ in range(maxPackets):
            try:
                data, _ = self.udpSocket.recvfrom(MAX_DATAGRAM_SIZE)
            except BlockingIOError:
                break
            except ConnectionResetError:
                continue
            frame = self.handleDatagram(data)
            if frame is not None:
                newest = frame
        self.reassembler.expire(self.clock())
        return newest

    def handleDatagram(self, data: bytes) -> Frame | None:
        nowNs = self.clock()
        if len(data) >= CommonHeader.SIZE:
            header = CommonHeader.unpack(data)
            if header.magic == MAGIC and header.type != PacketType.VIDEO_FRAGMENT:
                self.otherPackets += 1
                return None
        staleBefore = self.reassembler.stats.staleFragments
        frame = self.reassembler.push(data, nowNs)
        if frame is None and self.reassembler.stats.staleFragments > staleBefore:
            if nowNs - self.lastDeliveryNs >= SESSION_TIMEOUT_NS:
                self.reassembler.reset()
                self.senderRestarts += 1
                frame = self.reassembler.push(data, nowNs)
        if frame is not None:
            self.lastDeliveryNs = nowNs
        return frame


class FrameViewer:
    def __init__(self, receiver: FrameReceiver):
        self.receiver = receiver
        self.root = tkinter.Tk()
        host, port = receiver.address
        self.root.title(f"FreeMonitor debug viewer - {host}:{port}")
        self.root.geometry(INITIAL_WINDOW_SIZE)
        self.root.configure(background="black")
        self.root.protocol("WM_DELETE_WINDOW", self.root.destroy)
        self.statusLabel = tkinter.Label(self.root, anchor="w", font="TkFixedFont", text="Waiting for frames")
        self.statusLabel.pack(side="bottom", fill="x")
        self.imageLabel = tkinter.Label(self.root, background="black")
        self.imageLabel.pack(side="top", fill="both", expand=True)
        self.photo: ImageTk.PhotoImage | None = None
        self.lastFrame: Frame | None = None
        self.lastImageSize: tuple[int, int] | None = None
        self.framesSinceStatus = 0
        self.decodeErrors = 0
        self.unsupportedFrames = 0
        self.lastStatusNs = time.monotonic_ns()

    def run(self) -> None:
        self.root.after(POLL_INTERVAL_MS, self.poll)
        self.root.mainloop()

    def poll(self) -> None:
        frame = self.receiver.receivePending()
        if frame is not None:
            self.show(frame)
        nowNs = time.monotonic_ns()
        if nowNs - self.lastStatusNs >= STATUS_INTERVAL_NS:
            self.updateStatus(nowNs)
        self.root.after(POLL_INTERVAL_MS, self.poll)

    def show(self, frame: Frame) -> None:
        self.framesSinceStatus += 1
        self.lastFrame = frame
        self.lastImageSize = None
        if frame.codec != Codec.JPEG:
            self.unsupportedFrames += 1
            return
        image = decodeJPEG(frame.data)
        if image is None:
            self.decodeErrors += 1
            return
        self.lastImageSize = image.size
        fitted = fitImage(image, max(1, self.imageLabel.winfo_width()), max(1, self.imageLabel.winfo_height()))
        self.photo = ImageTk.PhotoImage(fitted)
        self.imageLabel.configure(image=self.photo)

    def updateStatus(self, nowNs: int) -> None:
        fps = self.framesSinceStatus * 1e9 / (nowNs - self.lastStatusNs)
        self.framesSinceStatus = 0
        self.lastStatusNs = nowNs
        stats = self.receiver.reassembler.stats
        parts = [
            f"{fps:5.1f} fps",
            f"delivered {stats.deliveredFrames}",
            f"discarded {stats.discardedFrames}",
            f"stale {stats.staleFragments}",
            f"duplicate {stats.duplicateFragments}",
            f"invalid {stats.invalidPackets}",
            f"other {self.receiver.otherPackets}",
            f"decode errors {self.decodeErrors}",
            f"not JPEG {self.unsupportedFrames}",
            f"restarts {self.receiver.senderRestarts}",
        ]
        if self.lastFrame is not None:
            codec = Codec(self.lastFrame.codec).name
            keyframe = " keyframe" if self.lastFrame.flags & FragmentFlags.KEYFRAME else ""
            size = f"{self.lastImageSize[0]}x{self.lastImageSize[1]} " if self.lastImageSize else ""
            parts.append(f"last: {size}{codec}{keyframe} {len(self.lastFrame.data) / 1024:.1f} KiB")
        self.statusLabel.configure(text=" | ".join(parts))


def parseArguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Shows FreeMonitor development-mode video (session ID 0).")
    parser.add_argument("--port", type=int, default=HOST_PORT, help=f"UDP port to receive on (default {HOST_PORT})")
    parser.add_argument("--bind", default="0.0.0.0", help="local IPv4 address to receive on")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parseArguments(argv)
    receiver = FrameReceiver((arguments.bind, arguments.port))
    warning = receiveBufferWarning(receiver.receiveBufferSize)
    if warning is not None:
        print(warning, file=sys.stderr)
    try:
        FrameViewer(receiver).run()
    finally:
        receiver.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
