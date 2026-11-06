#include "ReedSolomon.hpp"

#include <array>
#include <utility>

namespace fm {
    namespace {
        constexpr unsigned FIELD_SIZE = 256;
        constexpr unsigned FIELD_POLYNOMIAL = 0x11D;

        using ByteTable = std::array<std::uint8_t, FIELD_SIZE>;
        using ParityMatrix = std::array<std::array<std::uint8_t, MAX_FEC_GROUP_PARITY>, MAX_FEC_GROUP_PARITY>;

        struct FieldTables {
            std::array<ByteTable, FIELD_SIZE> products{};
            ByteTable inverses{};
            std::array<std::array<std::uint8_t, MAX_FEC_GROUP_SIZE>, MAX_FEC_GROUP_PARITY> coefficients{};
        };

        std::uint8_t multiplyByShifting(unsigned left, unsigned right) noexcept {
            unsigned product = 0;
            while (right) {
                if (right & 1) {
                    product ^= left;
                }
                left <<= 1;
                if (left & FIELD_SIZE) {
                    left ^= FIELD_POLYNOMIAL;
                }

                right >>= 1;
            }
            return static_cast<std::uint8_t>(product);
        }

        FieldTables makeFieldTables() noexcept {
            FieldTables tables;
            for (unsigned left = 0; left < FIELD_SIZE; ++left) {
                for (unsigned right = 0; right < FIELD_SIZE; ++right) {
                    const std::uint8_t product = multiplyByShifting(left, right);
                    tables.products[left][right] = product;
                    if (product == 1) {
                        tables.inverses[left] = static_cast<std::uint8_t>(right);
                    }
                }
            }
            for (std::size_t row = 0; row < MAX_FEC_GROUP_PARITY; ++row) {
                for (std::size_t column = 0; column < MAX_FEC_GROUP_SIZE; ++column) {
                    tables.coefficients[row][column] = tables.inverses[(MAX_FEC_GROUP_SIZE + row) ^ column];
                }
            }
            return tables;
        }

        const FieldTables TABLES = makeFieldTables();

        void addScaled(std::span<std::uint8_t> target, std::span<const std::uint8_t> source,
                       std::uint8_t factor) noexcept {
            const ByteTable& products = TABLES.products[factor];
            const std::size_t size = std::min(target.size(), source.size());
            for (std::size_t index = 0; index < size; ++index) {
                target[index] ^= products[source[index]];
            }
        }

        std::uint16_t fragmentCountOf(std::size_t frameSize) noexcept {
            return static_cast<std::uint16_t>((frameSize + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);
        }

        template<typename Byte>
        std::span<Byte> fragmentBytes(std::span<Byte> frameData, std::size_t fragIndex) noexcept {
            const std::size_t offset = fragIndex * MAX_FRAGMENT_PAYLOAD;
            return frameData.subspan(offset, std::min<std::size_t>(MAX_FRAGMENT_PAYLOAD, frameData.size() - offset));
        }

        bool invert(ParityMatrix& matrix, ParityMatrix& inverse, std::size_t size) noexcept {
            for (std::size_t row = 0; row < size; ++row) {
                inverse[row].fill(0);
                inverse[row][row] = 1;
            }
            for (std::size_t pivot = 0; pivot < size; ++pivot) {
                std::size_t pivotRow = pivot;
                while (pivotRow < size && matrix[pivotRow][pivot] == 0) {
                    ++pivotRow;
                }
                if (pivotRow == size) {
                    return false;
                }
                std::swap(matrix[pivot], matrix[pivotRow]);
                std::swap(inverse[pivot], inverse[pivotRow]);

                const ByteTable& scale = TABLES.products[TABLES.inverses[matrix[pivot][pivot]]];
                for (std::size_t column = 0; column < size; ++column) {
                    matrix[pivot][column] = scale[matrix[pivot][column]];
                    inverse[pivot][column] = scale[inverse[pivot][column]];
                }
                for (std::size_t row = 0; row < size; ++row) {
                    if (row == pivot || matrix[row][pivot] == 0) {
                        continue;
                    }
                    const ByteTable& factor = TABLES.products[matrix[row][pivot]];
                    for (std::size_t column = 0; column < size; ++column) {
                        matrix[row][column] ^= factor[matrix[pivot][column]];
                        inverse[row][column] ^= factor[inverse[pivot][column]];
                    }
                }
            }
            return true;
        }
    }

    std::uint8_t gfMultiply(std::uint8_t left, std::uint8_t right) noexcept {
        return TABLES.products[left][right];
    }

    std::uint8_t gfInverse(std::uint8_t value) noexcept {
        return TABLES.inverses[value];
    }

    std::uint8_t parityCoefficient(std::size_t row, std::size_t column) noexcept {
        return TABLES.coefficients[row][column];
    }

    bool computeParity(std::span<const std::uint8_t> frameData, std::uint16_t parityIndex,
                       std::span<std::uint8_t> parity) noexcept {
        const std::uint16_t fragCount = fragmentCountOf(frameData.size());
        const std::uint16_t groupCount = fecGroupCount(fragCount);
        if (groupCount == 0 || parity.size() != fecBlockSize(static_cast<std::uint32_t>(frameData.size()))) {
            return false;
        }
        const std::size_t group = parityIndex % groupCount;
        const std::size_t row = parityIndex / groupCount;
        if (row >= MAX_FEC_GROUP_PARITY) {
            return false;
        }

        std::ranges::fill(parity, std::uint8_t{0});
        for (std::size_t fragIndex = group; fragIndex < fragCount; fragIndex += groupCount) {
            addScaled(parity, fragmentBytes(frameData, fragIndex), TABLES.coefficients[row][fragIndex / groupCount]);
        }
        return true;
    }

    bool recoverFragments(std::span<std::uint8_t> frameData, std::span<const std::uint16_t> missingFragments,
                          std::span<const ParityBlock> parityBlocks) noexcept {
        const std::size_t count = missingFragments.size();
        const std::uint16_t fragCount = fragmentCountOf(frameData.size());
        const std::uint16_t groupCount = fecGroupCount(fragCount);
        if (count == 0 || count != parityBlocks.size() || count > MAX_FEC_GROUP_PARITY || groupCount == 0) {
            return false;
        }
        const std::size_t group = missingFragments.front() % groupCount;
        const std::size_t blockSize = fecBlockSize(static_cast<std::uint32_t>(frameData.size()));

        ParityMatrix matrix{};
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t row = parityBlocks[index].parityIndex / groupCount;
            if (parityBlocks[index].bytes.size() != blockSize ||
                parityBlocks[index].parityIndex % groupCount != group || row >= MAX_FEC_GROUP_PARITY) {
                return false;
            }
            for (std::size_t fragIndex = group; fragIndex < fragCount; fragIndex += groupCount) {
                if (std::ranges::find(missingFragments, fragIndex) == missingFragments.end()) {
                    addScaled(parityBlocks[index].bytes, fragmentBytes(frameData, fragIndex),
                              TABLES.coefficients[row][fragIndex / groupCount]);
                }
            }
            for (std::size_t column = 0; column < count; ++column) {
                const std::uint16_t fragIndex = missingFragments[column];
                if (fragIndex >= fragCount || fragIndex % groupCount != group) {
                    return false;
                }
                matrix[index][column] = TABLES.coefficients[row][fragIndex / groupCount];
            }
        }

        ParityMatrix inverse{};
        if (!invert(matrix, inverse, count)) {
            return false;
        }
        for (std::size_t column = 0; column < count; ++column) {
            const std::span<std::uint8_t> fragment = fragmentBytes(frameData, missingFragments[column]);
            std::ranges::fill(fragment, std::uint8_t{0});
            for (std::size_t index = 0; index < count; ++index) {
                addScaled(fragment, parityBlocks[index].bytes, inverse[column][index]);
            }
        }
        return true;
    }
}
