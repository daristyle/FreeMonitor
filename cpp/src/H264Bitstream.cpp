#include "H264Bitstream.hpp"

#include <cstring>

namespace fm {
    namespace {
        constexpr std::size_t START_CODE_SIZE = 3;
        constexpr std::uint8_t NAL_TYPE_MASK = 0x1F;
        constexpr std::uint8_t FILLER_DATA = 12;

        std::size_t findStartCode(std::span<const std::uint8_t> data, std::size_t from) noexcept {
            for (std::size_t i = from; i + START_CODE_SIZE <= data.size(); ++i) {
                if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                    return i;
                }
            }
            return data.size();
        }
    }

    std::size_t removeFillerData(std::span<std::uint8_t> accessUnit) noexcept {
        std::size_t startCode = findStartCode(accessUnit, 0);
        if (startCode == accessUnit.size()) {
            return accessUnit.size();
        }

        std::size_t written = 0;
        std::size_t unitBegin = 0;
        while (startCode < accessUnit.size()) {
            const std::size_t header = startCode + START_CODE_SIZE;
            const std::size_t nextStartCode = findStartCode(accessUnit, header);
            std::size_t unitEnd = nextStartCode;
            if (nextStartCode < accessUnit.size()) {
                while (unitEnd > header && accessUnit[unitEnd - 1] == 0) {
                    --unitEnd;
                }
            }
            const bool filler = header < accessUnit.size() && (accessUnit[header] & NAL_TYPE_MASK) == FILLER_DATA;
            if (!filler) {
                std::memmove(accessUnit.data() + written, accessUnit.data() + unitBegin, unitEnd - unitBegin);
                written += unitEnd - unitBegin;
            }
            unitBegin = unitEnd;
            startCode = nextStartCode;
        }
        return written;
    }
}
