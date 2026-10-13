import string
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

PYTHON_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = PYTHON_ROOT.parent
SCHEMA_PATH = REPO_ROOT / "spec" / "packets.toml"
CPP_OUTPUT_PATH = REPO_ROOT / "cpp" / "include" / "Packets.hpp"
PYTHON_OUTPUT_PATH = PYTHON_ROOT / "freemonitor" / "Packets.py"

PACKET_TYPE_DEFAULT = "PACKET_TYPE"
PAYLOAD_BASE_CONSTANT = "MAX_PACKET_SIZE"
PAYLOAD_CONSTANT_TYPE = "u32"
ENUM_TYPE = "u8"
FLAGS_TYPE = "u8"


@dataclass(frozen=True)
class FieldType:
    size: int
    structCode: str
    cppType: str
    minimum: int
    maximum: int


FIELD_TYPES = {
    "u8": FieldType(1, "B", "std::uint8_t", 0, 2**8 - 1),
    "u16": FieldType(2, "H", "std::uint16_t", 0, 2**16 - 1),
    "u32": FieldType(4, "I", "std::uint32_t", 0, 2**32 - 1),
    "u64": FieldType(8, "Q", "std::uint64_t", 0, 2**64 - 1),
    "i32": FieldType(4, "i", "std::int32_t", -(2**31), 2**31 - 1),
}


class SchemaError(ValueError):
    pass


@dataclass(frozen=True)
class Constant:
    name: str
    typeName: str
    value: int


@dataclass(frozen=True)
class NamedValues:
    name: str
    values: tuple[tuple[str, int], ...]


@dataclass(frozen=True)
class Field:
    name: str
    typeName: str
    offset: int
    default: int | str


@dataclass(frozen=True)
class Header:
    name: str
    memberName: str
    after: str | None
    offset: int
    size: int
    fields: tuple[Field, ...]

    def usesPacketType(self) -> bool:
        return any(field.default == PACKET_TYPE_DEFAULT for field in self.fields)


@dataclass(frozen=True)
class Packet:
    name: str
    className: str
    typeValue: int
    headers: tuple[Header, ...]
    fields: tuple[Field, ...]
    payload: str | None
    size: int

    def allFields(self) -> list[tuple[Header | None, Field]]:
        headerFields = [(header, field) for header in self.headers for field in header.fields]
        return headerFields + [(None, field) for field in self.fields]


@dataclass(frozen=True)
class Schema:
    constants: tuple[Constant, ...]
    enums: tuple[NamedValues, ...]
    flags: tuple[NamedValues, ...]
    headers: tuple[Header, ...]
    packets: tuple[Packet, ...]


def readSchemaFile(path: Path = SCHEMA_PATH) -> dict:
    with path.open("rb") as schemaFile:
        return tomllib.load(schemaFile)


def loadSchema(path: Path = SCHEMA_PATH) -> Schema:
    return parseSchema(readSchemaFile(path))


def parseSchema(data: dict) -> Schema:
    checkKeys("schema", data, {"constants", "enums", "flags", "headers", "packets"})
    constants = {name: parseConstant(name, entry) for name, entry in data.get("constants", {}).items()}
    enums = tuple(parseNamedValues(name, entries, ENUM_TYPE) for name, entries in data.get("enums", {}).items())
    flags = tuple(parseFlags(name, entries) for name, entries in data.get("flags", {}).items())
    headers = parseHeaders(data.get("headers", {}), constants)
    packets = tuple(parsePacket(name, entry, headers, constants) for name, entry in data.get("packets", {}).items())

    checkUnique("packet type value", [packet.typeValue for packet in packets])
    typeNames = ["PacketType"] + [named.name for named in enums + flags] + list(headers)
    checkUnique("type name", typeNames + [packet.className for packet in packets])
    checkUnique("packet name", [packet.name for packet in packets])

    for packet in packets:
        if packet.payload is not None:
            constants[packet.payload] = derivePayloadConstant(packet, constants)
        maxPacketSize = requireConstant(packet.name, PAYLOAD_BASE_CONSTANT, constants).value
        if packet.size > maxPacketSize:
            raise SchemaError(f"{packet.name}: {packet.size} bytes is larger than {PAYLOAD_BASE_CONSTANT}")

    return Schema(tuple(constants.values()), enums, flags, tuple(headers.values()), packets)


def parseConstant(name: str, entry: dict) -> Constant:
    checkKeys(name, entry, {"type", "value"})
    typeName = requireFieldType(name, requireKey(name, entry, "type"))
    value = requireKey(name, entry, "value")
    checkInRange(name, value, typeName)
    return Constant(name, typeName, value)


def parseNamedValues(name: str, entries: dict, typeName: str) -> NamedValues:
    for valueName, value in entries.items():
        checkInRange(f"{name}.{valueName}", value, typeName)
    checkUnique(f"value in {name}", list(entries.values()))
    return NamedValues(name, tuple(entries.items()))


def parseFlags(name: str, entries: dict) -> NamedValues:
    bitCount = FIELD_TYPES[FLAGS_TYPE].size * 8
    for flagName, bit in entries.items():
        if not isInteger(bit) or not 0 <= bit < bitCount:
            raise SchemaError(f"{name}.{flagName}: bit must be from 0 to {bitCount - 1}, got {bit!r}")
    checkUnique(f"bit in {name}", list(entries.values()))
    return NamedValues(name, tuple((flagName, 1 << bit) for flagName, bit in entries.items()))


def parseHeaders(entries: dict, constants: dict[str, Constant]) -> dict[str, Header]:
    headers: dict[str, Header] = {}
    for name, entry in entries.items():
        checkKeys(name, entry, {"after", "fields"})
        after = entry.get("after")
        offset = 0
        if after is not None:
            if after not in headers:
                raise SchemaError(f"{name}: 'after' must name a header defined above it, got {after!r}")
            offset = headers[after].offset + headers[after].size
        fields, end = parseFields(name, requireKey(name, entry, "fields"), offset, constants)
        headers[name] = Header(name, lowerCamel(name), after, offset, end - offset, fields)
    return headers


def parsePacket(name: str, entry: dict, headers: dict[str, Header], constants: dict[str, Constant]) -> Packet:
    checkKeys(name, entry, {"className", "type", "header", "payload", "fields"})
    className = requireKey(name, entry, "className")
    typeValue = requireKey(name, entry, "type")
    checkInRange(f"{name}.type", typeValue, ENUM_TYPE)
    payload = entry.get("payload")
    if payload is not None and not isinstance(payload, str):
        raise SchemaError(f"{name}: 'payload' must be a constant name, got {payload!r}")
    headerChain = headerChainFor(name, requireKey(name, entry, "header"), headers)
    start = headerChain[-1].offset + headerChain[-1].size
    fields, end = parseFields(name, requireKey(name, entry, "fields"), start, constants)
    return Packet(name, className, typeValue, headerChain, fields, payload, end)


def headerChainFor(packetName: str, headerName: object, headers: dict[str, Header]) -> tuple[Header, ...]:
    if headerName not in headers:
        raise SchemaError(f"{packetName}: unknown header {headerName!r}")
    chain = [headers[headerName]]
    while chain[0].after is not None:
        chain.insert(0, headers[chain[0].after])
    return tuple(chain)


def parseFields(
    owner: str, entries: list, offset: int, constants: dict[str, Constant]
) -> tuple[tuple[Field, ...], int]:
    fields = []
    for entry in entries:
        checkKeys(owner, entry, {"name", "type", "default"})
        name = requireKey(owner, entry, "name")
        where = f"{owner}.{name}"
        typeName = requireFieldType(where, requireKey(where, entry, "type"))
        fieldSize = FIELD_TYPES[typeName].size
        if offset % fieldSize != 0:
            raise SchemaError(f"{where}: offset {offset} is not a multiple of its size {fieldSize}")
        default = entry.get("default", 0)
        checkDefault(where, default, typeName, constants)
        fields.append(Field(name, typeName, offset, default))
        offset += fieldSize
    checkUnique(f"field name in {owner}", [field.name for field in fields])
    return tuple(fields), offset


def checkDefault(where: str, default: object, typeName: str, constants: dict[str, Constant]) -> None:
    if default == PACKET_TYPE_DEFAULT:
        if typeName != ENUM_TYPE:
            raise SchemaError(f"{where}: {PACKET_TYPE_DEFAULT} needs type {ENUM_TYPE}, got {typeName}")
    elif isinstance(default, str):
        constant = requireConstant(where, default, constants)
        if constant.typeName != typeName:
            raise SchemaError(f"{where}: constant {default} is {constant.typeName}, but the field is {typeName}")
    else:
        checkInRange(where, default, typeName)


def derivePayloadConstant(packet: Packet, constants: dict[str, Constant]) -> Constant:
    if packet.payload in constants:
        raise SchemaError(f"{packet.name}: payload constant {packet.payload} is already defined")
    value = requireConstant(packet.name, PAYLOAD_BASE_CONSTANT, constants).value - packet.size
    if value <= 0:
        raise SchemaError(f"{packet.name}: no room left for a payload")
    return Constant(packet.payload, PAYLOAD_CONSTANT_TYPE, value)


def requireKey(where: str, entry: dict, key: str) -> object:
    if key not in entry:
        raise SchemaError(f"{where}: missing {key!r}")
    return entry[key]


def requireFieldType(where: str, typeName: object) -> str:
    if typeName not in FIELD_TYPES:
        raise SchemaError(f"{where}: unknown type {typeName!r}, expected one of {', '.join(FIELD_TYPES)}")
    return typeName


def requireConstant(where: str, name: str, constants: dict[str, Constant]) -> Constant:
    if name not in constants:
        raise SchemaError(f"{where}: unknown constant {name!r}")
    return constants[name]


def checkKeys(where: str, entry: dict, allowed: set[str]) -> None:
    unknown = set(entry) - allowed
    if unknown:
        raise SchemaError(f"{where}: unknown keys {sorted(unknown)}")


def checkInRange(where: str, value: object, typeName: str) -> None:
    fieldType = FIELD_TYPES[typeName]
    if not isInteger(value) or not fieldType.minimum <= value <= fieldType.maximum:
        raise SchemaError(f"{where}: {value!r} does not fit in {typeName}")


def checkUnique(what: str, values: list) -> None:
    seen = set()
    for value in values:
        if value in seen:
            raise SchemaError(f"duplicate {what}: {value!r}")
        seen.add(value)


def isInteger(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def lowerCamel(name: str) -> str:
    upperCount = len(name) - len(name.lstrip(string.ascii_uppercase))
    if upperCount == len(name):
        return name.lower()
    if upperCount <= 1:
        return name[:1].lower() + name[1:]
    return name[: upperCount - 1].lower() + name[upperCount - 1 :]


def structFormat(fields: list[Field]) -> str:
    return "!" + "".join(FIELD_TYPES[field.typeName].structCode for field in fields)


def cppLiteral(value: int) -> str:
    return f"{value}ULL" if value > 2**63 - 1 else str(value)


def cppDefault(field: Field) -> str:
    if field.default == PACKET_TYPE_DEFAULT:
        return "0"
    return str(field.default)


def cppFieldAccess(header: Header | None, field: Field) -> str:
    return field.name if header is None else f"{header.memberName}.{field.name}"


def cppPointer(name: str, offset: int) -> str:
    return name if offset == 0 else f"{name} + {offset}"


def generateCPP(schema: Schema) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <span>",
        "#include <type_traits>",
        "",
        "namespace fm {",
    ]
    for constant in schema.constants:
        cppType = FIELD_TYPES[constant.typeName].cppType
        lines.append(f"    constexpr {cppType} {constant.name} = {cppLiteral(constant.value)};")
    lines.append("")

    packetTypes = [(packet.name, packet.typeValue) for packet in schema.packets]
    lines += cppEnum("PacketType", packetTypes, lambda value: f"0x{value:02X}")
    for enum in schema.enums:
        lines += cppEnum(enum.name, list(enum.values), str)
    for flags in schema.flags:
        lines.append(f"    struct {flags.name} {{")
        for flagName, mask in flags.values:
            lines.append(f"        static constexpr {FIELD_TYPES[FLAGS_TYPE].cppType} {flagName} = 0x{mask:02X};")
        lines += ["    };", ""]

    lines += [
        "    template<typename T>",
        "    constexpr void writeBigEndian(std::uint8_t* destination, T value) noexcept {",
        "        auto bits = static_cast<std::make_unsigned_t<T>>(value);",
        "        for (std::size_t index = 0; index < sizeof(T); ++index) {",
        "            destination[sizeof(T) - 1 - index] = static_cast<std::uint8_t>(bits >> (8 * index));",
        "        }",
        "    }",
        "",
        "    template<typename T>",
        "    constexpr T readBigEndian(const std::uint8_t* source) noexcept {",
        "        std::make_unsigned_t<T> bits = 0;",
        "        for (std::size_t index = 0; index < sizeof(T); ++index) {",
        "            bits = static_cast<std::make_unsigned_t<T>>((bits << 8) | source[index]);",
        "        }",
        "        return static_cast<T>(bits);",
        "    }",
        "",
    ]

    for header in schema.headers:
        lines += cppHeader(header)
    for packet in schema.packets:
        lines += cppPacket(packet)

    lines[-1] = "}"
    return "\n".join(lines) + "\n"


def cppEnum(name: str, values: list[tuple[str, int]], formatValue) -> list[str]:
    lines = [f"    enum class {name} : {FIELD_TYPES[ENUM_TYPE].cppType} {{"]
    lines += [f"        {valueName} = {formatValue(value)}," for valueName, value in values]
    return lines + ["    };", ""]


def cppHeader(header: Header) -> list[str]:
    lines = [
        f"    struct {header.name} {{",
        f"        static constexpr std::size_t OFFSET = {header.offset};",
        f"        static constexpr std::size_t SIZE = {header.size};",
        "",
    ]
    for field in header.fields:
        lines.append(f"        {FIELD_TYPES[field.typeName].cppType} {field.name} = {cppDefault(field)};")
    lines += [
        "",
        "        bool read(std::span<const std::uint8_t> buffer) noexcept {",
        "            if (buffer.size() < OFFSET + SIZE) {",
        "                return false;",
        "            }",
        "            const std::uint8_t* source = buffer.data();",
    ]
    for field in header.fields:
        cppType = FIELD_TYPES[field.typeName].cppType
        lines.append(f"            {field.name} = readBigEndian<{cppType}>({cppPointer('source', field.offset)});")
    lines += ["            return true;", "        }", "    };", ""]

    if header.usesPacketType():
        lines.append(f"    constexpr {header.name} make{header.name}(PacketType packetType) noexcept {{")
        lines.append(f"        {header.name} {header.memberName};")
        for field in header.fields:
            if field.default == PACKET_TYPE_DEFAULT:
                cppType = FIELD_TYPES[field.typeName].cppType
                lines.append(f"        {header.memberName}.{field.name} = static_cast<{cppType}>(packetType);")
        lines += [f"        return {header.memberName};", "    }", ""]
    return lines


def cppPacket(packet: Packet) -> list[str]:
    lines = [
        f"    struct {packet.className} {{",
        f"        static constexpr std::size_t SIZE = {packet.size};",
        "",
    ]
    for header in packet.headers:
        if header.usesPacketType():
            lines.append(f"        {header.name} {header.memberName} = make{header.name}(PacketType::{packet.name});")
        else:
            lines.append(f"        {header.name} {header.memberName};")
    for field in packet.fields:
        lines.append(f"        {FIELD_TYPES[field.typeName].cppType} {field.name} = {cppDefault(field)};")
    if packet.payload is not None:
        lines.append("        std::span<const std::uint8_t> payload;")

    lines += ["", "        std::size_t write(std::span<std::uint8_t> buffer) const noexcept {"]
    if packet.payload is None:
        lines.append("            if (buffer.size() < SIZE) {")
    else:
        lines.append("            if (buffer.size() < SIZE || buffer.size() - SIZE < payload.size()) {")
    lines += ["                return 0;", "            }", "            std::uint8_t* destination = buffer.data();"]
    for header, field in packet.allFields():
        destination = cppPointer("destination", field.offset)
        lines.append(f"            writeBigEndian({destination}, {cppFieldAccess(header, field)});")
    if packet.payload is None:
        lines.append("            return SIZE;")
    else:
        lines += [
            "            std::ranges::copy(payload, destination + SIZE);",
            "            return SIZE + payload.size();",
        ]
    lines += ["        }", ""]

    lines += [
        "        bool read(std::span<const std::uint8_t> buffer) noexcept {",
        "            if (buffer.size() < SIZE) {",
        "                return false;",
        "            }",
        "            const std::uint8_t* source = buffer.data();",
    ]
    for header, field in packet.allFields():
        cppType = FIELD_TYPES[field.typeName].cppType
        source = cppPointer("source", field.offset)
        lines.append(f"            {cppFieldAccess(header, field)} = readBigEndian<{cppType}>({source});")
    if packet.payload is not None:
        lines.append("            payload = buffer.subspan(SIZE);")
    lines += ["            return true;", "        }", "    };", ""]
    return lines


def pythonDefault(field: Field) -> str:
    if field.default == PACKET_TYPE_DEFAULT:
        return "0"
    return str(field.default)


def generatePython(schema: Schema) -> str:
    lines = [
        "import struct",
        "from dataclasses import dataclass, field",
        "from enum import IntEnum, IntFlag",
        "from typing import ClassVar",
        "",
    ]
    lines += [f"{constant.name} = {constant.value}" for constant in schema.constants]
    lines += ["", ""]

    lines += pythonEnum(
        "PacketType", "IntEnum", [(packet.name, f"0x{packet.typeValue:02X}") for packet in schema.packets]
    )
    for enum in schema.enums:
        lines += pythonEnum(enum.name, "IntEnum", [(name, str(value)) for name, value in enum.values])
    for flags in schema.flags:
        lines += pythonEnum(flags.name, "IntFlag", [(name, f"0x{mask:02X}") for name, mask in flags.values])

    for header in schema.headers:
        lines += pythonHeader(header)
    for packet in schema.packets:
        lines += pythonPacket(packet)

    while lines[-1] == "":
        lines.pop()
    return "\n".join(lines) + "\n"


def pythonEnum(name: str, base: str, values: list[tuple[str, str]]) -> list[str]:
    lines = [f"class {name}({base}):"]
    lines += [f"    {valueName} = {value}" for valueName, value in values]
    return lines + ["", ""]


def pythonHeader(header: Header) -> list[str]:
    lines = [
        "@dataclass",
        f"class {header.name}:",
        f"    OFFSET: ClassVar[int] = {header.offset}",
        f"    SIZE: ClassVar[int] = {header.size}",
        f'    FORMAT: ClassVar[struct.Struct] = struct.Struct("{structFormat(list(header.fields))}")',
        "",
    ]
    lines += [f"    {field.name}: int = {pythonDefault(field)}" for field in header.fields]
    lines += [
        "",
        "    @classmethod",
        f"    def unpack(cls, data: bytes) -> {header.name}:",
        "        if len(data) < cls.OFFSET + cls.SIZE:",
        f'            raise ValueError(f"{header.name} needs {{cls.OFFSET + cls.SIZE}} bytes, got {{len(data)}}")',
        "        return cls(*cls.FORMAT.unpack_from(data, cls.OFFSET))",
        "",
        "",
    ]
    return lines


def pythonPacket(packet: Packet) -> list[str]:
    allFields = [field for _, field in packet.allFields()]
    lines = [
        "@dataclass",
        f"class {packet.className}:",
        f"    SIZE: ClassVar[int] = {packet.size}",
        f'    FORMAT: ClassVar[struct.Struct] = struct.Struct("{structFormat(allFields)}")',
        "",
    ]
    for header in packet.headers:
        if header.usesPacketType():
            typeField = next(field for field in header.fields if field.default == PACKET_TYPE_DEFAULT)
            factory = f"lambda: {header.name}({typeField.name}=PacketType.{packet.name})"
            lines.append(f"    {header.memberName}: {header.name} = field(default_factory={factory})")
        else:
            lines.append(f"    {header.memberName}: {header.name} = field(default_factory={header.name})")
    lines += [f"    {field.name}: int = {pythonDefault(field)}" for field in packet.fields]
    if packet.payload is not None:
        lines.append('    payload: bytes = b""')

    lines += ["", "    def pack(self) -> bytes:"]
    lines.append(
        "        packed = self.FORMAT.pack(" if packet.payload is not None else "        return self.FORMAT.pack("
    )
    for header, field in packet.allFields():
        access = field.name if header is None else f"{header.memberName}.{field.name}"
        lines.append(f"            self.{access},")
    lines.append("        )")
    if packet.payload is not None:
        lines.append("        return packed + self.payload")

    lines += [
        "",
        "    @classmethod",
        f"    def unpack(cls, data: bytes) -> {packet.className}:",
        "        if len(data) < cls.SIZE:",
        f'            raise ValueError(f"{packet.className} needs {{cls.SIZE}} bytes, got {{len(data)}}")',
        "        values = cls.FORMAT.unpack_from(data)",
        "        return cls(",
    ]
    start = 0
    for header in packet.headers:
        end = start + len(header.fields)
        lines.append(f"            {header.name}(*values[{start}:{end}]),")
        start = end
    if packet.fields:
        lines.append(f"            *values[{start}:],")
    if packet.payload is not None:
        lines.append("            payload=bytes(data[cls.SIZE :]),")
    lines += ["        )", "", ""]
    return lines


def writeOutputs(schema: Schema) -> None:
    CPP_OUTPUT_PATH.write_text(generateCPP(schema), encoding="utf-8", newline="\n")
    PYTHON_OUTPUT_PATH.write_text(generatePython(schema), encoding="utf-8", newline="\n")


def main() -> int:
    try:
        schema = loadSchema()
    except SchemaError as error:
        print(f"Schema error: {error}", file=sys.stderr)
        return 1
    writeOutputs(schema)
    print(f"Wrote {CPP_OUTPUT_PATH.relative_to(REPO_ROOT)} and {PYTHON_OUTPUT_PATH.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
