import struct
from dataclasses import dataclass, field
from enum import IntEnum, IntFlag
from typing import ClassVar

MAGIC = 17997
PROTOCOL_VERSION = 1
MAX_PACKET_SIZE = 1400
MAX_FRAME_SIZE = 4194304
HOST_PORT = 50500
DISCOVERY_PORT = 50501
FRAGMENT_TIMEOUT_MS = 100
CURSOR_POS_MIN_INTERVAL_MS = 4
CURSOR_POS_REPEAT_INTERVAL_MS = 250
RELIABLE_RESEND_INTERVAL_MS = 100
HELLO_RESEND_INTERVAL_MS = 500
HEARTBEAT_INTERVAL_MS = 500
SESSION_TIMEOUT_MS = 3000
BYE_REPEAT_INTERVAL_MS = 20
KEYFRAME_REQUEST_IGNORE_MS = 100
KEYFRAME_REQUEST_REPEAT_INTERVAL_MS = 200
SHAPE_REQUEST_REPEAT_INTERVAL_MS = 500
STATS_INTERVAL_MS = 1000
DEVELOPMENT_KEYFRAME_INTERVAL_MS = 1000
MAX_INCOMPLETE_FRAMES = 4
MIN_SHAPE_CACHE_SIZE = 16
BYE_REPEAT_COUNT = 3
MAX_FRAGMENT_PAYLOAD = 1352
MAX_SHAPE_PAYLOAD = 1360


class PacketType(IntEnum):
    VIDEO_FRAGMENT = 0x01
    CURSOR_POS = 0x10
    CURSOR_SHAPE = 0x11
    HELLO = 0x20
    WELCOME = 0x21
    ACK = 0x22
    HEARTBEAT = 0x23
    KEYFRAME_REQUEST = 0x24
    SHAPE_REQUEST = 0x25
    STATS = 0x26
    BYE = 0x27
    REJECT = 0x28


class Codec(IntEnum):
    JPEG = 0
    H264 = 1


class RejectReason(IntEnum):
    UNSUPPORTED_VERSION = 1
    NO_COMMON_CODEC = 2
    INVALID_PARAMETERS = 3


class FragmentFlags(IntFlag):
    KEYFRAME = 0x01


class CursorFlags(IntFlag):
    VISIBLE = 0x01


class CodecFlags(IntFlag):
    JPEG = 0x01
    H264 = 0x02


class FeatureFlags(IntFlag):
    CURSOR_OVERLAY = 0x01


@dataclass
class CommonHeader:
    OFFSET: ClassVar[int] = 0
    SIZE: ClassVar[int] = 24
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQ")

    magic: int = MAGIC
    version: int = PROTOCOL_VERSION
    type: int = 0
    sessionId: int = 0
    seq: int = 0
    headerReserved: int = 0
    timestampUs: int = 0

    @classmethod
    def unpack(cls, data: bytes) -> CommonHeader:
        if len(data) < cls.OFFSET + cls.SIZE:
            raise ValueError(f"CommonHeader needs {cls.OFFSET + cls.SIZE} bytes, got {len(data)}")
        return cls(*cls.FORMAT.unpack_from(data, cls.OFFSET))


@dataclass
class ControlHeader:
    OFFSET: ClassVar[int] = 24
    SIZE: ClassVar[int] = 8
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!II")

    msgId: int = 0
    controlReserved: int = 0

    @classmethod
    def unpack(cls, data: bytes) -> ControlHeader:
        if len(data) < cls.OFFSET + cls.SIZE:
            raise ValueError(f"ControlHeader needs {cls.OFFSET + cls.SIZE} bytes, got {len(data)}")
        return cls(*cls.FORMAT.unpack_from(data, cls.OFFSET))


@dataclass
class VideoFragment:
    SIZE: ClassVar[int] = 48
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIHHIBBHQ")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.VIDEO_FRAGMENT))
    frameId: int = 0
    fragIndex: int = 0
    fragCount: int = 0
    frameSize: int = 0
    codec: int = 0
    flags: int = 0
    reserved: int = 0
    captureTimestampUs: int = 0
    payload: bytes = b""

    def pack(self) -> bytes:
        packed = self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.frameId,
            self.fragIndex,
            self.fragCount,
            self.frameSize,
            self.codec,
            self.flags,
            self.reserved,
            self.captureTimestampUs,
        )
        return packed + self.payload

    @classmethod
    def unpack(cls, data: bytes) -> VideoFragment:
        if len(data) < cls.SIZE:
            raise ValueError(f"VideoFragment needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            *values[7:],
            payload=bytes(data[cls.SIZE :]),
        )


@dataclass
class CursorPos:
    SIZE: ClassVar[int] = 40
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIiiBBH")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.CURSOR_POS))
    shapeId: int = 0
    x: int = 0
    y: int = 0
    flags: int = 0
    reserved0: int = 0
    reserved1: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.shapeId,
            self.x,
            self.y,
            self.flags,
            self.reserved0,
            self.reserved1,
        )

    @classmethod
    def unpack(cls, data: bytes) -> CursorPos:
        if len(data) < cls.SIZE:
            raise ValueError(f"CursorPos needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            *values[7:],
        )


@dataclass
class CursorShape:
    SIZE: ClassVar[int] = 40
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIHHHHHH")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.CURSOR_SHAPE))
    shapeId: int = 0
    fragIndex: int = 0
    fragCount: int = 0
    width: int = 0
    height: int = 0
    hotspotX: int = 0
    hotspotY: int = 0
    payload: bytes = b""

    def pack(self) -> bytes:
        packed = self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.shapeId,
            self.fragIndex,
            self.fragCount,
            self.width,
            self.height,
            self.hotspotX,
            self.hotspotY,
        )
        return packed + self.payload

    @classmethod
    def unpack(cls, data: bytes) -> CursorShape:
        if len(data) < cls.SIZE:
            raise ValueError(f"CursorShape needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            *values[7:],
            payload=bytes(data[cls.SIZE :]),
        )


@dataclass
class Hello:
    SIZE: ClassVar[int] = 40
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIIHHHBB")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.HELLO))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)
    width: int = 0
    height: int = 0
    refreshHz: int = 0
    codecs: int = 0
    features: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
            self.width,
            self.height,
            self.refreshHz,
            self.codecs,
            self.features,
        )

    @classmethod
    def unpack(cls, data: bytes) -> Hello:
        if len(data) < cls.SIZE:
            raise ValueError(f"Hello needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
            *values[9:],
        )


@dataclass
class Welcome:
    SIZE: ClassVar[int] = 40
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIIHHHBB")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.WELCOME))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)
    width: int = 0
    height: int = 0
    fps: int = 0
    codec: int = 0
    features: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
            self.width,
            self.height,
            self.fps,
            self.codec,
            self.features,
        )

    @classmethod
    def unpack(cls, data: bytes) -> Welcome:
        if len(data) < cls.SIZE:
            raise ValueError(f"Welcome needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
            *values[9:],
        )


@dataclass
class ACK:
    SIZE: ClassVar[int] = 36
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIII")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.ACK))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)
    ackedMsgId: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
            self.ackedMsgId,
        )

    @classmethod
    def unpack(cls, data: bytes) -> ACK:
        if len(data) < cls.SIZE:
            raise ValueError(f"ACK needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
            *values[9:],
        )


@dataclass
class Heartbeat:
    SIZE: ClassVar[int] = 48
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIIQII")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.HEARTBEAT))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)
    echoTimestampUs: int = 0
    echoDelayUs: int = 0
    reserved: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
            self.echoTimestampUs,
            self.echoDelayUs,
            self.reserved,
        )

    @classmethod
    def unpack(cls, data: bytes) -> Heartbeat:
        if len(data) < cls.SIZE:
            raise ValueError(f"Heartbeat needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
            *values[9:],
        )


@dataclass
class KeyframeRequest:
    SIZE: ClassVar[int] = 32
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQII")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.KEYFRAME_REQUEST))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
        )

    @classmethod
    def unpack(cls, data: bytes) -> KeyframeRequest:
        if len(data) < cls.SIZE:
            raise ValueError(f"KeyframeRequest needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
        )


@dataclass
class ShapeRequest:
    SIZE: ClassVar[int] = 36
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIII")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.SHAPE_REQUEST))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)
    shapeId: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
            self.shapeId,
        )

    @classmethod
    def unpack(cls, data: bytes) -> ShapeRequest:
        if len(data) < cls.SIZE:
            raise ValueError(f"ShapeRequest needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
            *values[9:],
        )


@dataclass
class Stats:
    SIZE: ClassVar[int] = 52
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIIIIIII")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.STATS))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)
    packetsReceived: int = 0
    packetsLost: int = 0
    framesCompleted: int = 0
    framesDropped: int = 0
    bytesReceived: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
            self.packetsReceived,
            self.packetsLost,
            self.framesCompleted,
            self.framesDropped,
            self.bytesReceived,
        )

    @classmethod
    def unpack(cls, data: bytes) -> Stats:
        if len(data) < cls.SIZE:
            raise ValueError(f"Stats needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
            *values[9:],
        )


@dataclass
class Bye:
    SIZE: ClassVar[int] = 32
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQII")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.BYE))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
        )

    @classmethod
    def unpack(cls, data: bytes) -> Bye:
        if len(data) < cls.SIZE:
            raise ValueError(f"Bye needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
        )


@dataclass
class Reject:
    SIZE: ClassVar[int] = 36
    FORMAT: ClassVar[struct.Struct] = struct.Struct("!HBBIIIQIIBBH")

    commonHeader: CommonHeader = field(default_factory=lambda: CommonHeader(type=PacketType.REJECT))
    controlHeader: ControlHeader = field(default_factory=ControlHeader)
    reason: int = 0
    version: int = 0
    reserved: int = 0

    def pack(self) -> bytes:
        return self.FORMAT.pack(
            self.commonHeader.magic,
            self.commonHeader.version,
            self.commonHeader.type,
            self.commonHeader.sessionId,
            self.commonHeader.seq,
            self.commonHeader.headerReserved,
            self.commonHeader.timestampUs,
            self.controlHeader.msgId,
            self.controlHeader.controlReserved,
            self.reason,
            self.version,
            self.reserved,
        )

    @classmethod
    def unpack(cls, data: bytes) -> Reject:
        if len(data) < cls.SIZE:
            raise ValueError(f"Reject needs {cls.SIZE} bytes, got {len(data)}")
        values = cls.FORMAT.unpack_from(data)
        return cls(
            CommonHeader(*values[0:7]),
            ControlHeader(*values[7:9]),
            *values[9:],
        )
