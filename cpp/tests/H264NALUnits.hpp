#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

constexpr std::uint8_t H264_NON_IDR_SLICE = 1;
constexpr std::uint8_t H264_IDR_SLICE = 5;
constexpr std::uint8_t H264_SPS = 7;
constexpr std::uint8_t H264_PPS = 8;
constexpr std::uint8_t H264_FILLER_DATA = 12;
constexpr std::uint8_t H264_MATRIX_BT709 = 1;
constexpr std::uint8_t H264_COLOUR_UNSPECIFIED = 2;
constexpr std::uint8_t H264_MATRIX_BT470BG = 5;
constexpr std::uint8_t H264_MATRIX_SMPTE170M = 6;

struct H264ColourDescription {
    bool present = false;
    bool fullRange = false;
    std::uint8_t primaries = H264_COLOUR_UNSPECIFIED;
    std::uint8_t transfer = H264_COLOUR_UNSPECIFIED;
    std::uint8_t matrix = H264_COLOUR_UNSPECIFIED;
};

class H264BitReader {
private:
    std::vector<std::uint8_t> bytes;
    std::size_t bitPosition = 0;

public:
    explicit H264BitReader(std::vector<std::uint8_t> bytes) : bytes(std::move(bytes)) {}

    std::uint32_t bits(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            const std::size_t byte = bitPosition / 8;
            const std::uint32_t bit = byte < bytes.size() ? (bytes[byte] >> (7 - bitPosition % 8)) & 1U : 0U;
            value = (value << 1) | bit;
            ++bitPosition;
        }
        return value;
    }

    std::uint32_t unsignedExpGolomb() {
        constexpr int MAX_LEADING_ZEROS = 31;
        int leadingZeros = 0;
        while (bits(1) == 0 && leadingZeros < MAX_LEADING_ZEROS) {
            ++leadingZeros;
        }
        return ((1U << leadingZeros) - 1) + bits(leadingZeros);
    }

    void skipExpGolomb() {
        unsignedExpGolomb();
    }
};

inline bool startsWithStartCode(std::span<const std::uint8_t> data) {
    return (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
           (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

inline std::vector<std::uint8_t> nalUnitTypes(std::span<const std::uint8_t> accessUnit) {
    constexpr std::uint8_t NAL_TYPE_MASK = 0x1F;
    std::vector<std::uint8_t> types;
    for (std::size_t i = 0; i + 3 < accessUnit.size(); ++i) {
        if (accessUnit[i] == 0 && accessUnit[i + 1] == 0 && accessUnit[i + 2] == 1) {
            types.push_back(static_cast<std::uint8_t>(accessUnit[i + 3] & NAL_TYPE_MASK));
            i += 2;
        }
    }
    return types;
}

inline bool containsNALUnit(const std::vector<std::uint8_t>& types, std::uint8_t type) {
    return std::ranges::find(types, type) != types.end();
}

inline std::optional<std::vector<std::uint8_t>> spsPayload(std::span<const std::uint8_t> accessUnit) {
    constexpr std::uint8_t NAL_TYPE_MASK = 0x1F;
    constexpr std::uint8_t EMULATION_PREVENTION = 3;
    for (std::size_t i = 0; i + 3 < accessUnit.size(); ++i) {
        if (accessUnit[i] != 0 || accessUnit[i + 1] != 0 || accessUnit[i + 2] != 1 ||
            (accessUnit[i + 3] & NAL_TYPE_MASK) != H264_SPS) {
            continue;
        }
        std::vector<std::uint8_t> payload;
        std::size_t zeros = 0;
        for (std::size_t j = i + 4; j < accessUnit.size(); ++j) {
            if (zeros >= 2 && accessUnit[j] == 1) {
                break;
            }
            if (zeros >= 2 && accessUnit[j] == EMULATION_PREVENTION) {
                zeros = 0;
                continue;
            }
            zeros = accessUnit[j] == 0 ? zeros + 1 : 0;
            payload.push_back(accessUnit[j]);
        }
        return payload;
    }
    return std::nullopt;
}

inline void skipScalingList(H264BitReader& reader, int size) {
    constexpr int SCALE_MODULO = 256;
    int lastScale = 8;
    int nextScale = 8;
    for (int j = 0; j < size; ++j) {
        if (nextScale != 0) {
            const std::uint32_t code = reader.unsignedExpGolomb();
            const int delta = (code & 1U) != 0 ? static_cast<int>((code + 1) / 2) : -static_cast<int>(code / 2);
            nextScale = (lastScale + delta + SCALE_MODULO) % SCALE_MODULO;
        }
        lastScale = nextScale == 0 ? lastScale : nextScale;
    }
}

inline std::optional<H264ColourDescription> spsColourDescription(std::span<const std::uint8_t> accessUnit) {
    constexpr std::array<std::uint32_t, 13> HIGH_PROFILES{100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    constexpr std::uint32_t CHROMA_444 = 3;
    constexpr std::uint32_t EXTENDED_SAR = 255;
    const auto payload = spsPayload(accessUnit);
    if (!payload) {
        return std::nullopt;
    }
    H264BitReader reader(*payload);
    const std::uint32_t profile = reader.bits(8);
    reader.bits(16);
    reader.skipExpGolomb();
    if (std::ranges::find(HIGH_PROFILES, profile) != HIGH_PROFILES.end()) {
        const std::uint32_t chromaFormat = reader.unsignedExpGolomb();
        if (chromaFormat == CHROMA_444) {
            reader.bits(1);
        }
        reader.skipExpGolomb();
        reader.skipExpGolomb();
        reader.bits(1);
        if (reader.bits(1) != 0) {
            const int listCount = chromaFormat != CHROMA_444 ? 8 : 12;
            for (int i = 0; i < listCount; ++i) {
                if (reader.bits(1) != 0) {
                    skipScalingList(reader, i < 6 ? 16 : 64);
                }
            }
        }
    }
    reader.skipExpGolomb();
    const std::uint32_t pictureOrderCountType = reader.unsignedExpGolomb();
    if (pictureOrderCountType == 0) {
        reader.skipExpGolomb();
    } else if (pictureOrderCountType == 1) {
        reader.bits(1);
        reader.skipExpGolomb();
        reader.skipExpGolomb();
        const std::uint32_t cycleLength = reader.unsignedExpGolomb();
        for (std::uint32_t i = 0; i < cycleLength; ++i) {
            reader.skipExpGolomb();
        }
    }
    reader.skipExpGolomb();
    reader.bits(1);
    reader.skipExpGolomb();
    reader.skipExpGolomb();
    if (reader.bits(1) == 0) {
        reader.bits(1);
    }
    reader.bits(1);
    if (reader.bits(1) != 0) {
        for (int i = 0; i < 4; ++i) {
            reader.skipExpGolomb();
        }
    }

    H264ColourDescription colours;
    if (reader.bits(1) == 0) {
        return colours;
    }
    if (reader.bits(1) != 0 && reader.bits(8) == EXTENDED_SAR) {
        reader.bits(32);
    }
    if (reader.bits(1) != 0) {
        reader.bits(1);
    }
    if (reader.bits(1) != 0) {
        reader.bits(3);
        colours.fullRange = reader.bits(1) != 0;
        if (reader.bits(1) != 0) {
            colours.present = true;
            colours.primaries = static_cast<std::uint8_t>(reader.bits(8));
            colours.transfer = static_cast<std::uint8_t>(reader.bits(8));
            colours.matrix = static_cast<std::uint8_t>(reader.bits(8));
        }
    }
    return colours;
}
