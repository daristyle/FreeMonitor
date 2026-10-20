import argparse
import math
import select
import socket
import struct
import time
from collections import Counter
from contextlib import contextmanager

import pytest

from tools.LossEmulator import Impairment, LossEmulator, parseAddress, parseMilliseconds, parseRate

LOCALHOST = "127.0.0.1"
QUIET_TIMEOUT_S = 0.3
PACKET_FORMAT = struct.Struct("!I")


@contextmanager
def emulatorSetup(impairment, seed=1):
    target = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    target.bind((LOCALHOST, 0))
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.bind((LOCALHOST, 0))
    emulator = LossEmulator((LOCALHOST, 0), target.getsockname(), impairment, seed)
    emulator.start()
    try:
        yield client, emulator, target
    finally:
        emulator.close()
        client.close()
        target.close()


def sendNumbers(client, emulator, count):
    for number in range(count):
        client.sendto(PACKET_FORMAT.pack(number), emulator.listenAddress)


def receiveNumbers(udpSocket):
    numbers = []
    udpSocket.settimeout(QUIET_TIMEOUT_S)
    try:
        while True:
            data, _ = udpSocket.recvfrom(65535)
            numbers.append(PACKET_FORMAT.unpack(data)[0])
    except TimeoutError:
        return numbers


def testForwardsEverythingInOrderWithoutImpairment():
    with emulatorSetup(Impairment()) as (client, emulator, target):
        sendNumbers(client, emulator, 300)
        assert receiveNumbers(target) == list(range(300))
        assert emulator.forwardStats.sent == 300


def testLossRateDropsAboutThatFraction():
    with emulatorSetup(Impairment(lossRate=0.3)) as (client, emulator, target):
        sendNumbers(client, emulator, 1000)
        received = receiveNumbers(target)
        assert 600 <= len(received) <= 800
        assert emulator.forwardStats.dropped == 1000 - len(received)


def testLossRateOneDropsEverything():
    with emulatorSetup(Impairment(lossRate=1.0)) as (client, emulator, target):
        sendNumbers(client, emulator, 50)
        assert receiveNumbers(target) == []
        assert emulator.forwardStats.dropped == 50


def testDuplicateRateOneSendsEveryPacketTwice():
    with emulatorSetup(Impairment(duplicateRate=1.0)) as (client, emulator, target):
        sendNumbers(client, emulator, 50)
        assert Counter(receiveNumbers(target)) == {number: 2 for number in range(50)}


def testDelayHoldsPacketsBack():
    with emulatorSetup(Impairment(delayMs=50)) as (client, emulator, target):
        target.settimeout(1.0)
        sentAt = time.monotonic()
        sendNumbers(client, emulator, 1)
        target.recvfrom(65535)
        assert time.monotonic() - sentAt >= 0.05


def testReorderingChangesOrderButKeepsEveryPacket():
    with emulatorSetup(Impairment(reorderRate=0.2, reorderDelayMs=20)) as (client, emulator, target):
        for number in range(200):
            client.sendto(PACKET_FORMAT.pack(number), emulator.listenAddress)
            time.sleep(0.001)
        received = receiveNumbers(target)
        assert sorted(received) == list(range(200))
        assert received != list(range(200))


def testRepliesGoBackToTheClient():
    with emulatorSetup(Impairment()) as (client, emulator, target):
        sendNumbers(client, emulator, 1)
        target.settimeout(1.0)
        _, emulatorAddress = target.recvfrom(65535)
        target.sendto(PACKET_FORMAT.pack(77), emulatorAddress)
        assert receiveNumbers(client) == [77]
        assert emulator.returnStats.sent == 1


def testSameSeedGivesSameLoss():
    def survivingNumbers():
        with emulatorSetup(Impairment(lossRate=0.5), seed=5) as (client, emulator, target):
            sendNumbers(client, emulator, 200)
            return receiveNumbers(target)

    assert survivingNumbers() == survivingNumbers()


def testReceiveAllReadsAtMostMaxPacketsPerCall():
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.bind((LOCALHOST, 0))
    try:
        with LossEmulator((LOCALHOST, 0), client.getsockname(), Impairment()) as emulator:
            sendNumbers(client, emulator, 5)
            stats = emulator.forwardStats
            while stats.received < 5 and select.select([emulator.listenSocket], [], [], QUIET_TIMEOUT_S)[0]:
                receivedBefore = stats.received
                emulator.receiveAll(emulator.listenSocket, maxPackets=2)
                assert stats.received - receivedBefore <= 2
    finally:
        client.close()
    assert stats.received == 5


@pytest.mark.parametrize(
    "arguments",
    [
        {"lossRate": 1.5},
        {"duplicateRate": -0.1},
        {"delayMs": -1},
        {"reorderDelayMs": -5},
        {"lossRate": math.nan},
        {"delayMs": math.inf},
        {"jitterMs": math.nan},
    ],
)
def testImpairmentRejectsInvalidValues(arguments):
    with pytest.raises(ValueError):
        Impairment(**arguments)


def testParseAddress():
    assert parseAddress("127.0.0.1:50500") == ("127.0.0.1", 50500)
    for text in ["127.0.0.1", ":50500", "host:port", "host:70000"]:
        with pytest.raises(argparse.ArgumentTypeError):
            parseAddress(text)


def testParseRate():
    assert parseRate("0.05") == 0.05
    for text in ["-0.1", "1.5", "abc", "nan", "inf"]:
        with pytest.raises(argparse.ArgumentTypeError):
            parseRate(text)


def testParseMilliseconds():
    assert parseMilliseconds("20") == 20.0
    for text in ["-1", "abc", "nan", "inf"]:
        with pytest.raises(argparse.ArgumentTypeError):
            parseMilliseconds(text)
