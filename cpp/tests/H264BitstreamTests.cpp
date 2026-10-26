#include "H264Bitstream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace {
    using Bytes = std::vector<std::uint8_t>;

    const Bytes SPS{0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28};
    const Bytes PPS{0, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80};
    const Bytes IDR_SLICE{0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33};
    const Bytes P_SLICE{0, 0, 1, 0x41, 0x9A, 0x0C, 0x21};
    const Bytes FILLER{0, 0, 1, 0x0C, 0xFF, 0xFF, 0xFF, 0x80};
    const Bytes FILLER_WITH_ZERO_BYTE{0, 0, 0, 1, 0x0C, 0xFF, 0xFF, 0x80};

    Bytes join(const std::vector<Bytes>& units) {
        Bytes joined;
        for (const Bytes& unit : units) {
            joined.insert(joined.end(), unit.begin(), unit.end());
        }
        return joined;
    }

    Bytes removeFiller(Bytes accessUnit) {
        accessUnit.resize(fm::removeFillerData(accessUnit));
        return accessUnit;
    }
}

TEST_CASE("removeFillerData leaves an access unit without filler data unchanged") {
    const Bytes accessUnit = join({SPS, PPS, IDR_SLICE});
    CHECK(removeFiller(accessUnit) == accessUnit);
}

TEST_CASE("removeFillerData removes filler data between and after slices") {
    CHECK(removeFiller(join({SPS, PPS, FILLER, IDR_SLICE})) == join({SPS, PPS, IDR_SLICE}));
    CHECK(removeFiller(join({P_SLICE, FILLER})) == P_SLICE);
    CHECK(removeFiller(join({P_SLICE, FILLER, FILLER_WITH_ZERO_BYTE, FILLER})) == P_SLICE);
}

TEST_CASE("removeFillerData keeps the zero byte of the unit after the filler data") {
    CHECK(removeFiller(join({P_SLICE, FILLER, SPS, PPS, IDR_SLICE})) == join({P_SLICE, SPS, PPS, IDR_SLICE}));
    CHECK(removeFiller(join({FILLER_WITH_ZERO_BYTE, SPS})) == SPS);
}

TEST_CASE("removeFillerData only looks at the type of a unit, not at bytes inside it") {
    const Bytes sliceWithFillerByte{0, 0, 1, 0x41, 0x0C, 0x0C, 0xFF};
    CHECK(removeFiller(join({sliceWithFillerByte, FILLER})) == sliceWithFillerByte);
}

TEST_CASE("removeFillerData leaves data without a start code unchanged") {
    CHECK(removeFiller({}).empty());
    const Bytes noStartCode{0x0C, 0xFF, 0x80};
    CHECK(removeFiller(noStartCode) == noStartCode);
}
