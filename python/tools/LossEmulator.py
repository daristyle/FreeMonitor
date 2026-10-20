import argparse
import heapq
import math
import random
import select
import socket
import sys
import threading
import time
from dataclasses import dataclass

RECEIVE_BUFFER_SIZE = 8 * 1024 * 1024
MAX_DATAGRAM_SIZE = 65535
MAX_PACKETS_PER_STEP = 1000
IDLE_WAIT_S = 0.1
REPORT_INTERVAL_NS = 1_000_000_000
STOP_JOIN_TIMEOUT_S = 2.0


@dataclass(frozen=True)
class Impairment:
    lossRate: float = 0.0
    delayMs: float = 0.0
    jitterMs: float = 0.0
    reorderRate: float = 0.0
    reorderDelayMs: float = 0.0
    duplicateRate: float = 0.0

    def __post_init__(self):
        for name in ("lossRate", "reorderRate", "duplicateRate"):
            if not 0.0 <= getattr(self, name) <= 1.0:
                raise ValueError(f"{name} must be between 0 and 1, got {getattr(self, name)}")
        for name in ("delayMs", "jitterMs", "reorderDelayMs"):
            if not math.isfinite(getattr(self, name)) or getattr(self, name) < 0.0:
                raise ValueError(f"{name} must be a finite number of at least 0, got {getattr(self, name)}")


@dataclass
class DirectionStats:
    received: int = 0
    dropped: int = 0
    duplicated: int = 0
    sent: int = 0
    sendErrors: int = 0

    def summary(self) -> str:
        return (
            f"{self.received} in, {self.dropped} lost, {self.duplicated} duplicated, "
            f"{self.sent} out, {self.sendErrors} send errors"
        )


def openSocket(address: tuple[str, int]) -> socket.socket:
    udpSocket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udpSocket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RECEIVE_BUFFER_SIZE)
    udpSocket.bind(address)
    udpSocket.setblocking(False)
    return udpSocket


class LossEmulator:
    def __init__(
        self,
        listenAddress: tuple[str, int],
        targetAddress: tuple[str, int],
        impairment: Impairment,
        seed: int | None = None,
    ):
        self.impairment = impairment
        self.random = random.Random(seed)
        self.targetAddress = (socket.gethostbyname(targetAddress[0]), targetAddress[1])
        self.clientAddress: tuple[str, int] | None = None
        self.listenSocket = openSocket(listenAddress)
        self.forwardSocket = openSocket(("0.0.0.0", 0))
        self.pending: list[tuple[int, int, socket.socket, bytes, tuple[str, int], DirectionStats]] = []
        self.scheduledCount = 0
        self.forwardStats = DirectionStats()
        self.returnStats = DirectionStats()
        self.stopRequested = threading.Event()
        self.thread: threading.Thread | None = None

    def __enter__(self) -> LossEmulator:
        return self

    def __exit__(self, *exceptionInfo) -> None:
        self.close()

    @property
    def listenAddress(self) -> tuple[str, int]:
        return self.listenSocket.getsockname()

    def start(self) -> None:
        self.thread = threading.Thread(target=self.serveForever, name="LossEmulator", daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.stopRequested.set()
        if self.thread is not None:
            self.thread.join(STOP_JOIN_TIMEOUT_S)
            self.thread = None

    def close(self) -> None:
        self.stop()
        self.listenSocket.close()
        self.forwardSocket.close()

    def serveForever(self) -> None:
        while not self.stopRequested.is_set():
            self.step(IDLE_WAIT_S)

    def step(self, maxWaitS: float) -> None:
        readable, _, _ = select.select(
            [self.listenSocket, self.forwardSocket], [], [], self.timeUntilNextSend(maxWaitS)
        )
        for udpSocket in readable:
            self.receiveAll(udpSocket)
        self.sendDue(time.monotonic_ns())

    def timeUntilNextSend(self, maxWaitS: float) -> float:
        if not self.pending:
            return maxWaitS
        return min(maxWaitS, max(0.0, (self.pending[0][0] - time.monotonic_ns()) / 1e9))

    def receiveAll(self, udpSocket: socket.socket, maxPackets: int = MAX_PACKETS_PER_STEP) -> None:
        for _ in range(maxPackets):
            try:
                data, address = udpSocket.recvfrom(MAX_DATAGRAM_SIZE)
            except BlockingIOError:
                return
            except ConnectionResetError:
                continue
            nowNs = time.monotonic_ns()
            if udpSocket is self.listenSocket:
                self.clientAddress = address
                self.schedule(data, self.forwardSocket, self.targetAddress, self.forwardStats, nowNs)
            elif self.clientAddress is not None:
                self.schedule(data, self.listenSocket, self.clientAddress, self.returnStats, nowNs)

    def schedule(
        self,
        data: bytes,
        udpSocket: socket.socket,
        address: tuple[str, int],
        stats: DirectionStats,
        nowNs: int,
    ) -> None:
        stats.received += 1
        if self.random.random() < self.impairment.lossRate:
            stats.dropped += 1
            return
        copies = 1
        if self.random.random() < self.impairment.duplicateRate:
            copies = 2
            stats.duplicated += 1
        for _ in range(copies):
            entry = (nowNs + self.delayNs(), self.scheduledCount, udpSocket, data, address, stats)
            heapq.heappush(self.pending, entry)
            self.scheduledCount += 1

    def delayNs(self) -> int:
        delayMs = self.impairment.delayMs + self.random.uniform(-self.impairment.jitterMs, self.impairment.jitterMs)
        if self.random.random() < self.impairment.reorderRate:
            delayMs += self.impairment.reorderDelayMs
        return max(0, round(delayMs * 1_000_000))

    def sendDue(self, nowNs: int) -> None:
        while self.pending and self.pending[0][0] <= nowNs:
            _, _, udpSocket, data, address, stats = heapq.heappop(self.pending)
            try:
                udpSocket.sendto(data, address)
                stats.sent += 1
            except OSError:
                stats.sendErrors += 1

    def summary(self) -> str:
        return f"forward: {self.forwardStats.summary()} | return: {self.returnStats.summary()}"


def parseAddress(text: str) -> tuple[str, int]:
    host, separator, port = text.rpartition(":")
    if not separator or not host or not port.isdigit() or not 0 < int(port) < 65536:
        raise argparse.ArgumentTypeError(f"expected HOST:PORT, got {text!r}")
    return host, int(port)


def parseRate(text: str) -> float:
    try:
        rate = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected a number between 0 and 1, got {text!r}") from None
    if not 0.0 <= rate <= 1.0:
        raise argparse.ArgumentTypeError(f"expected a number between 0 and 1, got {text!r}")
    return rate


def parseMilliseconds(text: str) -> float:
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected milliseconds, got {text!r}") from None
    if not math.isfinite(value) or value < 0.0:
        raise argparse.ArgumentTypeError(f"expected a finite number of milliseconds of at least 0, got {text!r}")
    return value


def parseArguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="UDP proxy that adds loss, delay, reordering and duplication in both directions."
    )
    parser.add_argument("--listen", type=parseAddress, required=True, help="HOST:PORT the sender sends to")
    parser.add_argument("--target", type=parseAddress, required=True, help="HOST:PORT packets are forwarded to")
    parser.add_argument("--loss", type=parseRate, default=0.0, help="fraction of packets dropped, 0 to 1")
    parser.add_argument("--delay-ms", type=parseMilliseconds, default=0.0, help="fixed delay per packet")
    parser.add_argument("--jitter-ms", type=parseMilliseconds, default=0.0, help="random extra delay, plus or minus")
    parser.add_argument("--reorder", type=parseRate, default=0.0, help="fraction of packets held back")
    parser.add_argument(
        "--reorder-delay-ms", type=parseMilliseconds, default=10.0, help="how long a held-back packet waits"
    )
    parser.add_argument("--duplicate", type=parseRate, default=0.0, help="fraction of packets sent twice")
    parser.add_argument("--seed", type=int, default=None, help="random seed, for repeatable runs")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parseArguments(argv)
    impairment = Impairment(
        lossRate=arguments.loss,
        delayMs=arguments.delay_ms,
        jitterMs=arguments.jitter_ms,
        reorderRate=arguments.reorder,
        reorderDelayMs=arguments.reorder_delay_ms,
        duplicateRate=arguments.duplicate,
    )
    with LossEmulator(arguments.listen, arguments.target, impairment, arguments.seed) as emulator:
        host, port = emulator.listenAddress
        print(f"Forwarding {host}:{port} -> {arguments.target[0]}:{arguments.target[1]} with {impairment}")
        nextReportNs = time.monotonic_ns() + REPORT_INTERVAL_NS
        try:
            while True:
                emulator.step(IDLE_WAIT_S)
                if time.monotonic_ns() >= nextReportNs:
                    print(emulator.summary(), flush=True)
                    nextReportNs += REPORT_INTERVAL_NS
        except KeyboardInterrupt:
            print(emulator.summary())
    return 0


if __name__ == "__main__":
    sys.exit(main())
