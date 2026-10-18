import re

import pytest

from freemonitor import Packets
from tools.PacketGenerator import (
    CPP_OUTPUT_PATH,
    PYTHON_OUTPUT_PATH,
    REPO_ROOT,
    SchemaError,
    generateCPP,
    generatePython,
    loadSchema,
    parseSchema,
    readSchemaFile,
)

SPEC_PACKET_SIZES = {
    "VideoFragment": 48,
    "CursorPos": 40,
    "CursorShape": 40,
    "Hello": 40,
    "Welcome": 40,
    "ACK": 36,
    "Heartbeat": 48,
    "KeyframeRequest": 32,
    "ShapeRequest": 36,
    "Stats": 52,
    "Bye": 32,
    "Reject": 36,
}

SPEC_STRUCT_FORMATS = {
    "CommonHeader": "!HBBIIIQ",
    "VideoFragment": "!HBBIIIQIHHIBBHQ",
    "CursorPos": "!HBBIIIQIiiBBH",
    "CursorShape": "!HBBIIIQIHHHHHH",
}


@pytest.fixture
def schemaData():
    return readSchemaFile()


def testGeneratedFilesAreUpToDate():
    schema = loadSchema()
    assert CPP_OUTPUT_PATH.read_text(encoding="utf-8") == generateCPP(schema)
    assert PYTHON_OUTPUT_PATH.read_text(encoding="utf-8") == generatePython(schema)


@pytest.mark.parametrize(("className", "size"), SPEC_PACKET_SIZES.items())
def testPacketSizesMatchSpec(className, size):
    packetClass = getattr(Packets, className)
    assert packetClass.SIZE == size
    assert len(packetClass().pack()) == size


@pytest.mark.parametrize(("className", "structFormat"), SPEC_STRUCT_FORMATS.items())
def testStructFormatsMatchSpec(className, structFormat):
    assert getattr(Packets, className).FORMAT.format == structFormat


def readSpecConstants():
    text = (REPO_ROOT / "spec" / "protocol.md").read_text(encoding="utf-8")
    section = text.split("### 1.5 Constants", 1)[1].split("\n## ", 1)[0]
    constants = {}
    for name, valueText in re.findall(r"^\| `([A-Z0-9_]+)` \| ([^|]+) \|", section, re.MULTILINE):
        number = re.match(r"[\d,]+", valueText.split("=")[-1].strip())
        constants[name] = int(number.group().replace(",", ""))
    return constants


def testSpecConstantsTableIsRead():
    assert len(readSpecConstants()) == 22


def testConstantsMatchSpec():
    for name, value in readSpecConstants().items():
        assert getattr(Packets, name) == value, name
    assert Packets.MAGIC == 0x464D
    assert Packets.PROTOCOL_VERSION == 1


def testEveryPacketTypeHasOneClassWithHeaderDefaults():
    headers = [getattr(Packets, className)().commonHeader for className in SPEC_PACKET_SIZES]
    assert sorted(header.type for header in headers) == sorted(Packets.PacketType)
    assert all(header.magic == Packets.MAGIC for header in headers)
    assert all(header.version == Packets.PROTOCOL_VERSION for header in headers)


def testACKBytesAreBigEndian():
    ack = Packets.ACK(ackedMsgId=7)
    ack.commonHeader.sessionId = 0x01020304
    ack.commonHeader.seq = 5
    ack.commonHeader.timestampUs = 0x1122334455667788
    expected = bytes.fromhex("464d01220102030400000005000000001122334455667788000000000000000000000007")
    assert ack.pack() == expected


def testFeaturesByteIsAtOffset39OfHelloAndWelcome():
    assert Packets.FeatureFlags.CURSOR_OVERLAY == 0x01
    hello = Packets.Hello(codecs=Packets.CodecFlags.JPEG, features=Packets.FeatureFlags.CURSOR_OVERLAY)
    welcome = Packets.Welcome(features=Packets.FeatureFlags.CURSOR_OVERLAY)
    assert hello.pack()[39] == 0x01
    assert welcome.pack()[39] == 0x01


def testStatsBytesReceivedIsAtOffset48():
    assert Packets.Stats(bytesReceived=0x01020304).pack()[48:52] == bytes.fromhex("01020304")


def testNegativeCursorCoordinatesRoundTrip():
    cursorPos = Packets.CursorPos(shapeId=3, x=-20, y=-1, flags=Packets.CursorFlags.VISIBLE)
    data = cursorPos.pack()
    assert data[28:32] == bytes.fromhex("ffffffec")
    assert Packets.CursorPos.unpack(data) == cursorPos


@pytest.mark.parametrize("className", SPEC_PACKET_SIZES)
def testEveryPacketRoundTrips(className):
    packetClass = getattr(Packets, className)
    packet = packetClass()
    packet.commonHeader.sessionId = 42
    packet.commonHeader.timestampUs = 2**64 - 1
    assert packetClass.unpack(packet.pack()) == packet


def testVideoFragmentPayloadFollowsTheHeader():
    payload = bytes(range(256)) * 5
    fragment = Packets.VideoFragment(frameId=9, fragCount=1, frameSize=len(payload), payload=payload)
    data = fragment.pack()
    assert len(data) == Packets.VideoFragment.SIZE + len(payload)
    assert data[Packets.VideoFragment.SIZE :] == payload
    assert Packets.VideoFragment.unpack(data) == fragment


def testCommonHeaderCanBeReadFromAnyPacket():
    data = Packets.Stats(packetsLost=3).pack()
    header = Packets.CommonHeader.unpack(data)
    assert header.type == Packets.PacketType.STATS
    assert Packets.ControlHeader.unpack(data).msgId == 0


def testUnpackRejectsShortData():
    with pytest.raises(ValueError):
        Packets.ACK.unpack(bytes(35))
    with pytest.raises(ValueError):
        Packets.CommonHeader.unpack(bytes(23))


def testChangingAFieldUpdatesEveryLanguage(schemaData):
    schemaData["packets"]["ACK"]["fields"].append({"name": "extraCounter", "type": "u32"})
    schema = parseSchema(schemaData)
    ack = next(packet for packet in schema.packets if packet.className == "ACK")
    assert ack.size == 40
    assert "std::uint32_t extraCounter = 0;" in generateCPP(schema)
    assert "extraCounter: int = 0" in generatePython(schema)


def testPayloadConstantsFollowMaxPacketSize(schemaData):
    schemaData["constants"]["MAX_PACKET_SIZE"]["value"] = 1200
    constants = {constant.name: constant.value for constant in parseSchema(schemaData).constants}
    assert constants["MAX_FRAGMENT_PAYLOAD"] == 1200 - 48
    assert constants["MAX_SHAPE_PAYLOAD"] == 1200 - 40


def testMisalignedFieldIsRejected(schemaData):
    schemaData["packets"]["ACK"]["fields"].insert(0, {"name": "oneByte", "type": "u8"})
    with pytest.raises(SchemaError, match="not a multiple"):
        parseSchema(schemaData)


def testDuplicatePacketTypeIsRejected(schemaData):
    schemaData["packets"]["BYE"]["type"] = 0x22
    with pytest.raises(SchemaError, match="duplicate packet type value"):
        parseSchema(schemaData)


def testDefaultConstantMustHaveTheFieldType(schemaData):
    schemaData["headers"]["CommonHeader"]["fields"][0]["default"] = "MAX_PACKET_SIZE"
    with pytest.raises(SchemaError, match="is u32, but the field is u16"):
        parseSchema(schemaData)


def testValueOutOfRangeIsRejected(schemaData):
    schemaData["constants"]["HOST_PORT"]["value"] = 70000
    with pytest.raises(SchemaError, match="does not fit in u16"):
        parseSchema(schemaData)


def testPacketLargerThanMaxPacketSizeIsRejected(schemaData):
    schemaData["constants"]["MAX_PACKET_SIZE"]["value"] = 40
    with pytest.raises(SchemaError):
        parseSchema(schemaData)


def testUnknownKeyIsRejected(schemaData):
    schemaData["packets"]["ACK"]["fields"][0]["defualt"] = 1
    with pytest.raises(SchemaError, match="unknown keys"):
        parseSchema(schemaData)


def testHexConstantIsPrintedInHex():
    schema = loadSchema()
    assert "constexpr std::uint16_t MAGIC = 0x464D;" in generateCPP(schema)
    assert "\nMAGIC = 0x464D\n" in generatePython(schema)


def testHexConstantIsPaddedToItsTypeWidth(schemaData):
    schemaData["constants"]["HOST_PORT"]["hex"] = True
    schemaData["constants"]["MAX_PACKET_SIZE"]["hex"] = True
    schema = parseSchema(schemaData)
    assert "constexpr std::uint16_t HOST_PORT = 0xC544;" in generateCPP(schema)
    assert "\nMAX_PACKET_SIZE = 0x00000578\n" in generatePython(schema)


def testHexMustBeABoolean(schemaData):
    schemaData["constants"]["MAGIC"]["hex"] = "yes"
    with pytest.raises(SchemaError, match="'hex' must be true or false"):
        parseSchema(schemaData)


def testHexNeedsAnUnsignedType(schemaData):
    schemaData["constants"]["MAGIC"] = {"type": "i32", "value": 1, "hex": True}
    with pytest.raises(SchemaError, match="'hex' needs an unsigned type"):
        parseSchema(schemaData)
