package io.github.daristyle.freemonitor

data class VideoSize(
    val width: Int,
    val height: Int,
)

class NALUnitRange {
    var start = 0
    var end = 0

    val length: Int
        get() = end - start
}

object H264ParameterSets {
    const val NAL_TYPE_SPS = 7
    const val NAL_TYPE_PPS = 8

    private const val START_CODE_SIZE = 3
    private const val NAL_TYPE_MASK = 0x1F
    private const val BYTE_MASK = 0xFF
    private const val EMULATION_PREVENTION = 3
    private const val MACROBLOCK_SIZE = 16
    private const val CHROMA_420 = 1
    private const val CHROMA_422 = 2
    private const val CHROMA_444 = 3
    private const val MAX_POC_CYCLE_LENGTH = 255
    private val ANNEX_B_START_CODE = byteArrayOf(0, 0, 0, 1)
    private val HIGH_PROFILES = intArrayOf(100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)

    fun find(
        data: ByteArray,
        length: Int,
        type: Int,
        range: NALUnitRange,
    ): Boolean {
        var startCode = nextStartCode(data, 0, length)
        while (startCode < length) {
            val header = startCode + START_CODE_SIZE
            val next = nextStartCode(data, header, length)
            if (header < length && (data[header].toInt() and NAL_TYPE_MASK) == type) {
                var end = next
                if (next < length) {
                    while (end > header && data[end - 1].toInt() == 0) {
                        --end
                    }
                }
                range.start = header
                range.end = end
                return true
            }
            startCode = next
        }
        return false
    }

    fun withStartCode(
        data: ByteArray,
        range: NALUnitRange,
    ): ByteArray = ANNEX_B_START_CODE + data.copyOfRange(range.start, range.end)

    fun sameUnit(
        data: ByteArray,
        range: NALUnitRange,
        unitWithStartCode: ByteArray?,
    ): Boolean {
        if (unitWithStartCode == null || unitWithStartCode.size != ANNEX_B_START_CODE.size + range.length) {
            return false
        }
        for (i in 0 until range.length) {
            if (unitWithStartCode[ANNEX_B_START_CODE.size + i] != data[range.start + i]) {
                return false
            }
        }
        return true
    }

    fun size(
        data: ByteArray,
        sps: NALUnitRange,
    ): VideoSize? {
        val reader = BitReader(unescape(data, sps.start + 1, sps.end))
        val profile = reader.bits(8)
        reader.bits(16)
        reader.unsignedExpGolomb()
        var chromaFormat = CHROMA_420
        if (profile in HIGH_PROFILES) {
            chromaFormat = reader.unsignedExpGolomb()
            if (chromaFormat == CHROMA_444) {
                reader.bits(1)
            }
            reader.unsignedExpGolomb()
            reader.unsignedExpGolomb()
            reader.bits(1)
            if (reader.bits(1) == 1) {
                val listCount = if (chromaFormat != CHROMA_444) 8 else 12
                for (i in 0 until listCount) {
                    if (reader.bits(1) == 1) {
                        skipScalingList(reader, if (i < 6) 16 else 64)
                    }
                }
            }
        }
        reader.unsignedExpGolomb()
        when (reader.unsignedExpGolomb()) {
            0 -> {
                reader.unsignedExpGolomb()
            }

            1 -> {
                reader.bits(1)
                reader.signedExpGolomb()
                reader.signedExpGolomb()
                val cycleLength = reader.unsignedExpGolomb()
                if (cycleLength > MAX_POC_CYCLE_LENGTH) {
                    return null
                }
                repeat(cycleLength) { reader.signedExpGolomb() }
            }
        }
        reader.unsignedExpGolomb()
        reader.bits(1)
        val widthInMacroblocks = reader.unsignedExpGolomb() + 1
        val heightInMapUnits = reader.unsignedExpGolomb() + 1
        val frameMacroblocksOnly = reader.bits(1)
        if (frameMacroblocksOnly == 0) {
            reader.bits(1)
        }
        reader.bits(1)
        var cropLeft = 0
        var cropRight = 0
        var cropTop = 0
        var cropBottom = 0
        if (reader.bits(1) == 1) {
            cropLeft = reader.unsignedExpGolomb()
            cropRight = reader.unsignedExpGolomb()
            cropTop = reader.unsignedExpGolomb()
            cropBottom = reader.unsignedExpGolomb()
        }
        if (reader.overrun) {
            return null
        }
        val cropUnitX = if (chromaFormat == CHROMA_420 || chromaFormat == CHROMA_422) 2 else 1
        val cropUnitY = (if (chromaFormat == CHROMA_420) 2 else 1) * (2 - frameMacroblocksOnly)
        val width = widthInMacroblocks * MACROBLOCK_SIZE - (cropLeft + cropRight) * cropUnitX
        val height =
            (2 - frameMacroblocksOnly) * heightInMapUnits * MACROBLOCK_SIZE - (cropTop + cropBottom) * cropUnitY
        return if (width > 0 && height > 0) VideoSize(width, height) else null
    }

    private fun nextStartCode(
        data: ByteArray,
        from: Int,
        length: Int,
    ): Int {
        var i = from
        while (i + START_CODE_SIZE <= length) {
            if (data[i].toInt() == 0 && data[i + 1].toInt() == 0 && data[i + 2].toInt() == 1) {
                return i
            }
            ++i
        }
        return length
    }

    private fun unescape(
        data: ByteArray,
        start: Int,
        end: Int,
    ): ByteArray {
        val bytes = ByteArray(maxOf(0, end - start))
        var count = 0
        var zeros = 0
        for (i in start until end) {
            val value = data[i].toInt() and BYTE_MASK
            if (zeros >= 2 && value == EMULATION_PREVENTION) {
                zeros = 0
                continue
            }
            zeros = if (value == 0) zeros + 1 else 0
            bytes[count++] = data[i]
        }
        return bytes.copyOf(count)
    }

    private fun skipScalingList(
        reader: BitReader,
        size: Int,
    ) {
        var lastScale = 8
        var nextScale = 8
        repeat(size) {
            if (nextScale != 0) {
                nextScale = (lastScale + reader.signedExpGolomb() + 256) % 256
            }
            lastScale = if (nextScale == 0) lastScale else nextScale
        }
    }

    private class BitReader(
        private val bytes: ByteArray,
    ) {
        private var position = 0

        val overrun: Boolean
            get() = position > bytes.size * Byte.SIZE_BITS

        fun bits(count: Int): Int {
            var value = 0
            repeat(count) {
                val byte = position / Byte.SIZE_BITS
                val bit =
                    if (byte < bytes.size) {
                        (bytes[byte].toInt() shr (Byte.SIZE_BITS - 1 - position % Byte.SIZE_BITS)) and 1
                    } else {
                        0
                    }
                value = (value shl 1) or bit
                ++position
            }
            return value
        }

        fun unsignedExpGolomb(): Int {
            var leadingZeros = 0
            while (bits(1) == 0 && leadingZeros < MAX_LEADING_ZEROS) {
                ++leadingZeros
            }
            return ((1 shl leadingZeros) - 1) + bits(leadingZeros)
        }

        fun signedExpGolomb(): Int {
            val code = unsignedExpGolomb()
            return if ((code and 1) == 1) (code + 1) / 2 else -(code / 2)
        }

        private companion object {
            const val MAX_LEADING_ZEROS = 30
        }
    }
}
