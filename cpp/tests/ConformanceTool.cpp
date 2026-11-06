#include "Fragmenter.hpp"
#include "Packets.hpp"
#include "Reassembler.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;
    using Bytes = std::vector<std::uint8_t>;

    constexpr int BAD_INPUT = 2;
    constexpr std::size_t FRAGMENT_ARGUMENT_COUNT = 7;
    constexpr std::size_t PARITY_ARGUMENT_COUNT = 8;
    constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
    constexpr std::string_view USAGE =
            "usage: fm_conformance fragment <sessionId> <firstSeq> <frameId> <codec> <keyframe> <captureTimestampUs> "
            "<timestampUs> | parity <sessionId> <firstSeq> <frameId> <codec> <keyframe> <captureTimestampUs> "
            "<timestampUs> <parityCount> | reassemble <sessionId> | packet";

    int fail(std::string_view message) {
        std::println(stderr, "fm_conformance: {}", message);
        return BAD_INPUT;
    }

    template<std::integral Number>
    std::optional<Number> parseNumber(std::string_view text) {
        Number value{};
        const char* const end = text.data() + text.size();
        const auto [parsedEnd, error] = std::from_chars(text.data(), end, value);
        if (error != std::errc{} || parsedEnd != end) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<Bytes> parseHex(std::string_view text) {
        if (text.size() % 2 != 0) {
            return std::nullopt;
        }
        Bytes bytes;
        bytes.reserve(text.size() / 2);
        for (std::size_t index = 0; index < text.size(); index += 2) {
            std::uint8_t value = 0;
            const char* const end = text.data() + index + 2;
            const auto [parsedEnd, error] = std::from_chars(text.data() + index, end, value, 16);
            if (error != std::errc{} || parsedEnd != end) {
                return std::nullopt;
            }
            bytes.push_back(value);
        }
        return bytes;
    }

    std::string toHex(std::span<const std::uint8_t> bytes) {
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const std::uint8_t byte : bytes) {
            text.push_back(HEX_DIGITS[byte >> 4]);
            text.push_back(HEX_DIGITS[byte & 0x0F]);
        }
        return text;
    }

    bool isValidHeader(const fm::CommonHeader& header) noexcept {
        return header.magic == fm::MAGIC && header.version == fm::PROTOCOL_VERSION && header.headerReserved == 0;
    }

    int writeFrame(std::span<const std::string_view> arguments, std::optional<std::uint16_t> parityCount) {
        const auto sessionId = parseNumber<std::uint32_t>(arguments[0]);
        const auto firstSeq = parseNumber<std::uint32_t>(arguments[1]);
        const auto frameId = parseNumber<std::uint32_t>(arguments[2]);
        const auto codecValue = parseNumber<std::uint8_t>(arguments[3]);
        const auto codec = codecValue ? fm::toCodec(*codecValue) : std::nullopt;
        const auto keyframe = parseNumber<std::uint8_t>(arguments[4]);
        const auto captureTimestampUs = parseNumber<std::uint64_t>(arguments[5]);
        const auto timestampUs = parseNumber<std::uint64_t>(arguments[6]);
        if (!sessionId || !firstSeq || !frameId || !codec || !keyframe || *keyframe > 1 || !captureTimestampUs ||
            !timestampUs) {
            return fail(USAGE);
        }

        std::string line;
        if (!std::getline(std::cin, line)) {
            return fail("this command needs one line of frame data on stdin");
        }
        const auto frame = parseHex(line);
        if (!frame) {
            return fail("the frame data is not hex");
        }

        fm::Fragmenter fragmenter;
        if (!fragmenter.begin(*frame, *frameId, *codec, *keyframe == 1, *captureTimestampUs)) {
            std::println("INVALID");
            return 0;
        }
        std::array<std::uint8_t, fm::MAX_PACKET_SIZE> packet{};
        std::uint32_t seq = *firstSeq;
        const std::uint16_t packetCount = parityCount.value_or(fragmenter.fragmentCount());
        for (std::uint16_t index = 0; index < packetCount; ++index) {
            const std::size_t size =
                    parityCount ? fragmenter.writeParity(index, *parityCount, *sessionId, seq, *timestampUs, packet)
                                : fragmenter.write(index, *sessionId, seq, *timestampUs, packet);
            if (size == 0) {
                std::println("INVALID");
                return 0;
            }
            std::println("{}", toHex(std::span(packet).first(size)));
            ++seq;
        }
        return 0;
    }

    int runFragment(std::span<const std::string_view> arguments) {
        if (arguments.size() != FRAGMENT_ARGUMENT_COUNT) {
            return fail(USAGE);
        }
        return writeFrame(arguments, std::nullopt);
    }

    int runParity(std::span<const std::string_view> arguments) {
        if (arguments.size() != PARITY_ARGUMENT_COUNT) {
            return fail(USAGE);
        }
        const auto parityCount = parseNumber<std::uint16_t>(arguments.back());
        if (!parityCount) {
            return fail(USAGE);
        }
        return writeFrame(arguments.first(FRAGMENT_ARGUMENT_COUNT), parityCount);
    }

    void printStatus(const fm::Reassembler& reassembler, fm::FragmentStatus status) {
        switch (status) {
            case fm::FragmentStatus::ACCEPTED:
                std::println("ACCEPTED");
                break;
            case fm::FragmentStatus::DUPLICATE:
                std::println("DUPLICATE");
                break;
            case fm::FragmentStatus::TOO_OLD:
                std::println("TOO_OLD");
                break;
            case fm::FragmentStatus::INVALID:
                std::println("INVALID");
                break;
            case fm::FragmentStatus::FRAME_COMPLETE: {
                const fm::ReassembledFrame frame = reassembler.completedFrame();
                std::println("FRAME_COMPLETE {} {} {} {} {}", frame.frameId, static_cast<int>(frame.codec),
                             frame.keyframe ? 1 : 0, frame.captureTimestampUs, toHex(frame.data));
                break;
            }
        }
    }

    template<typename Packet>
    void acceptPacket(fm::Reassembler& reassembler, std::span<const std::uint8_t> bytes, Clock::time_point now) {
        Packet packet;
        if (!packet.read(bytes)) {
            std::println("INVALID");
            return;
        }
        printStatus(reassembler, reassembler.accept(packet, now));
    }

    void reassemblePacket(fm::Reassembler& reassembler, std::uint32_t sessionId, std::span<const std::uint8_t> packet,
                          Clock::time_point now) {
        fm::CommonHeader header;
        if (!header.read(packet) || !isValidHeader(header)) {
            std::println("INVALID");
            return;
        }
        if (header.sessionId != sessionId) {
            std::println("IGNORED");
            return;
        }
        if (header.type == static_cast<std::uint8_t>(fm::PacketType::VIDEO_FRAGMENT)) {
            acceptPacket<fm::VideoFragment>(reassembler, packet, now);
        } else if (header.type == static_cast<std::uint8_t>(fm::PacketType::VIDEO_PARITY)) {
            acceptPacket<fm::VideoParity>(reassembler, packet, now);
        } else {
            std::println("IGNORED");
        }
    }

    int runReassemble(std::span<const std::string_view> arguments) {
        const auto sessionId = arguments.size() == 1 ? parseNumber<std::uint32_t>(arguments[0]) : std::nullopt;
        if (!sessionId) {
            return fail(USAGE);
        }

        fm::Reassembler reassembler;
        Clock::time_point now{};
        std::string line;
        while (std::getline(std::cin, line)) {
            const std::string_view text = line;
            const std::size_t space = text.find(' ');
            if (space == std::string_view::npos) {
                return fail("reassemble needs <nowMs> <packet> on each line");
            }
            const auto nowMs = parseNumber<std::int64_t>(text.substr(0, space));
            const auto packet = parseHex(text.substr(space + 1));
            if (!nowMs || !packet) {
                return fail("reassemble needs a decimal time and a hex packet on each line");
            }
            now = Clock::time_point{} + std::chrono::milliseconds(*nowMs);
            reassemblePacket(reassembler, *sessionId, *packet, now);
        }
        reassembler.expire(now);
        std::println("TOTAL {} {}", reassembler.framesCompleted(), reassembler.framesDropped());
        return 0;
    }

    template<typename Packet>
    std::optional<Bytes> rewrite(std::span<const std::uint8_t> bytes) {
        Packet packet;
        if (!packet.read(bytes)) {
            return std::nullopt;
        }
        Bytes written(bytes.size());
        written.resize(packet.write(written));
        return written;
    }

    std::optional<Bytes> rewritePacket(std::span<const std::uint8_t> bytes) {
        fm::CommonHeader header;
        if (!header.read(bytes)) {
            return std::nullopt;
        }
        switch (static_cast<fm::PacketType>(header.type)) {
            case fm::PacketType::VIDEO_FRAGMENT:
                return rewrite<fm::VideoFragment>(bytes);
            case fm::PacketType::VIDEO_PARITY:
                return rewrite<fm::VideoParity>(bytes);
            case fm::PacketType::CURSOR_POS:
                return rewrite<fm::CursorPos>(bytes);
            case fm::PacketType::CURSOR_SHAPE:
                return rewrite<fm::CursorShape>(bytes);
            case fm::PacketType::HELLO:
                return rewrite<fm::Hello>(bytes);
            case fm::PacketType::WELCOME:
                return rewrite<fm::Welcome>(bytes);
            case fm::PacketType::ACK:
                return rewrite<fm::ACK>(bytes);
            case fm::PacketType::HEARTBEAT:
                return rewrite<fm::Heartbeat>(bytes);
            case fm::PacketType::KEYFRAME_REQUEST:
                return rewrite<fm::KeyframeRequest>(bytes);
            case fm::PacketType::SHAPE_REQUEST:
                return rewrite<fm::ShapeRequest>(bytes);
            case fm::PacketType::STATS:
                return rewrite<fm::Stats>(bytes);
            case fm::PacketType::BYE:
                return rewrite<fm::Bye>(bytes);
            case fm::PacketType::REJECT:
                return rewrite<fm::Reject>(bytes);
        }
        return std::nullopt;
    }

    int runPacket(std::span<const std::string_view> arguments) {
        if (!arguments.empty()) {
            return fail(USAGE);
        }
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto bytes = parseHex(line);
            if (!bytes) {
                return fail("the packet is not hex");
            }
            if (const auto written = rewritePacket(*bytes)) {
                std::println("{}", toHex(*written));
            } else {
                std::println("INVALID");
            }
        }
        return 0;
    }
}

int main(int argumentCount, char** arguments) {
    const std::vector<std::string_view> argumentList(arguments + 1, arguments + argumentCount);
    if (argumentList.empty()) {
        return fail(USAGE);
    }
    const std::string_view command = argumentList.front();
    const auto commandArguments = std::span(argumentList).subspan(1);
    if (command == "fragment") {
        return runFragment(commandArguments);
    }
    if (command == "parity") {
        return runParity(commandArguments);
    }
    if (command == "reassemble") {
        return runReassemble(commandArguments);
    }
    if (command == "packet") {
        return runPacket(commandArguments);
    }
    return fail(USAGE);
}
