#include "DiscoveryPackets.hpp"

namespace fm {
    namespace {
        constexpr std::uint8_t UTF8_CONTINUATION_MASK = 0xC0;
        constexpr std::uint8_t UTF8_CONTINUATION_BITS = 0x80;

        bool isContinuationByte(char byte) noexcept {
            return (static_cast<std::uint8_t>(byte) & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_BITS;
        }

        bool isDiscoveryHeader(const CommonHeader& header, PacketType type) noexcept {
            return header.magic == MAGIC && header.type == static_cast<std::uint8_t>(type) && header.sessionId == 0 &&
                   header.headerReserved == 0;
        }
    }

    std::string fitHostName(std::string_view name) {
        if (name.empty()) {
            return std::string(DEFAULT_HOST_NAME);
        }
        if (name.size() <= MAX_HOST_NAME_SIZE) {
            return std::string(name);
        }
        std::size_t cut = MAX_HOST_NAME_SIZE;
        while (cut > 0 && isContinuationByte(name[cut])) {
            --cut;
        }
        return cut == 0 ? std::string(DEFAULT_HOST_NAME) : std::string(name.substr(0, cut));
    }

    std::size_t writeDiscover(std::uint32_t nonce, std::uint32_t seq, std::uint64_t timestampUs,
                              std::span<std::uint8_t> buffer) noexcept {
        Discover discover;
        discover.commonHeader.seq = seq;
        discover.commonHeader.timestampUs = timestampUs;
        discover.nonce = nonce;
        return discover.write(buffer);
    }

    std::optional<std::uint32_t> readDiscover(std::span<const std::uint8_t> packet) noexcept {
        Discover discover;
        if (packet.size() != Discover::SIZE || !discover.read(packet) ||
            !isDiscoveryHeader(discover.commonHeader, PacketType::DISCOVER)) {
            return std::nullopt;
        }
        return discover.nonce;
    }

    std::size_t writeAnnounce(std::uint32_t nonce, std::uint16_t port, std::string_view name, std::uint32_t seq,
                              std::uint64_t timestampUs, std::span<std::uint8_t> buffer) noexcept {
        if (name.empty() || name.size() > MAX_HOST_NAME_SIZE || port == 0) {
            return 0;
        }
        Announce announce;
        announce.commonHeader.seq = seq;
        announce.commonHeader.timestampUs = timestampUs;
        announce.nonce = nonce;
        announce.port = port;
        announce.payload = std::span(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
        return announce.write(buffer);
    }

    std::optional<AnnounceInfo> readAnnounce(std::span<const std::uint8_t> packet, std::uint32_t nonce) noexcept {
        Announce announce;
        if (packet.size() <= Announce::SIZE || packet.size() > Announce::SIZE + MAX_HOST_NAME_SIZE ||
            !announce.read(packet) || !isDiscoveryHeader(announce.commonHeader, PacketType::ANNOUNCE) ||
            announce.nonce != nonce || announce.port == 0 || announce.reserved != 0) {
            return std::nullopt;
        }
        return AnnounceInfo{.port = announce.port,
                            .version = announce.commonHeader.version,
                            .name = std::string_view(reinterpret_cast<const char*>(announce.payload.data()),
                                                     announce.payload.size())};
    }
}
