package io.github.daristyle.freemonitor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class H264ParameterSetsTests {
    @Test
    fun theSizeComesFromTheSPSOfBothHostEncoders() {
        assertEquals(VideoSize(2944, 1840), sizeOf(NVENC_SPS_2944X1840))
        assertEquals(VideoSize(2944, 1840), sizeOf(VAAPI_SPS_2944X1840))
    }

    @Test
    fun croppingRemovesThePaddingOfOddSizes() {
        assertEquals(VideoSize(640, 360), sizeOf(NVENC_SPS_640X360))
        assertEquals(VideoSize(640, 360), sizeOf(VAAPI_SPS_640X360))
    }

    @Test
    fun theSPSAndPPSAreFoundInAKeyframe() {
        val keyframe = accessUnit(SPS_START_CODE + NVENC_SPS_2944X1840, PPS_START_CODE + PPS, IDR_START_CODE + IDR)
        val sps = NALUnitRange()
        val pps = NALUnitRange()
        assertTrue(H264ParameterSets.find(keyframe, keyframe.size, H264ParameterSets.NAL_TYPE_SPS, sps))
        assertTrue(H264ParameterSets.find(keyframe, keyframe.size, H264ParameterSets.NAL_TYPE_PPS, pps))
        assertArrayEquals(NVENC_SPS_2944X1840, keyframe.copyOfRange(sps.start, sps.end))
        assertArrayEquals(PPS, keyframe.copyOfRange(pps.start, pps.end))
        assertArrayEquals(byteArrayOf(0, 0, 0, 1) + PPS, H264ParameterSets.withStartCode(keyframe, pps))
    }

    @Test
    fun aUnitAtTheEndOfTheBufferIsFoundWithItsLastByte() {
        val codecConfig = accessUnit(SPS_START_CODE + NVENC_SPS_640X360, PPS_START_CODE + PPS)
        val pps = NALUnitRange()
        assertTrue(H264ParameterSets.find(codecConfig, codecConfig.size, H264ParameterSets.NAL_TYPE_PPS, pps))
        assertArrayEquals(PPS, codecConfig.copyOfRange(pps.start, pps.end))
    }

    @Test
    fun emulationPreventionBytesBeforeTheSizeAreRemoved() {
        val sps =
            syntheticSPS(MAIN_PROFILE) { writer ->
                writer.unsignedExpGolomb(1)
                writer.bits(0, 1)
                writer.signedExpGolomb(LARGE_POC_OFFSET)
                writer.signedExpGolomb(0)
                writer.unsignedExpGolomb(0)
            }
        assertTrue(containsEmulationPrevention(sps))
        assertEquals(VideoSize(1920, 1080), sizeOf(sps))
    }

    @Test
    fun scalingListsBeforeTheSizeAreSkippedEvenWhenTheyEndEarly() {
        val sps =
            syntheticSPS(HIGH_PROFILE, highProfileFields = { writer ->
                writer.unsignedExpGolomb(1)
                writer.unsignedExpGolomb(0)
                writer.unsignedExpGolomb(0)
                writer.bits(0, 1)
                writer.bits(1, 1)
                val lists = mapOf(0 to listOf(-8L), 1 to listOf(2L, 3L, -13L), 6 to List(64) { 0L })
                for (list in 0 until 8) {
                    val deltas = lists[list]
                    writer.bits(if (deltas != null) 1 else 0, 1)
                    deltas?.forEach(writer::signedExpGolomb)
                }
            })
        assertEquals(VideoSize(1920, 1080), sizeOf(sps))
    }

    @Test
    fun aDeltaFrameHasNoSPS() {
        val delta = accessUnit(IDR_START_CODE + byteArrayOf(0x41, 0x9A.toByte(), 0x21))
        assertFalse(H264ParameterSets.find(delta, delta.size, H264ParameterSets.NAL_TYPE_SPS, NALUnitRange()))
    }

    @Test
    fun sameUnitComparesWithTheConfiguredParameterSet() {
        val keyframe = accessUnit(SPS_START_CODE + NVENC_SPS_640X360, PPS_START_CODE + PPS)
        val sps = NALUnitRange()
        H264ParameterSets.find(keyframe, keyframe.size, H264ParameterSets.NAL_TYPE_SPS, sps)
        val configured = H264ParameterSets.withStartCode(keyframe, sps)
        assertTrue(H264ParameterSets.sameUnit(keyframe, sps, configured))
        assertFalse(H264ParameterSets.sameUnit(keyframe, sps, byteArrayOf(0, 0, 0, 1) + NVENC_SPS_2944X1840))
        assertFalse(H264ParameterSets.sameUnit(keyframe, sps, null))
    }

    @Test
    fun aTruncatedSPSHasNoSize() {
        assertNull(sizeOf(NVENC_SPS_2944X1840.copyOf(4)))
    }

    private fun syntheticSPS(
        profile: Int,
        highProfileFields: (BitWriter) -> Unit = {},
        pictureOrderCount: (BitWriter) -> Unit = { writer ->
            writer.unsignedExpGolomb(0)
            writer.unsignedExpGolomb(0)
        },
    ): ByteArray {
        val writer = BitWriter()
        writer.bits(profile.toLong(), 8)
        writer.bits(0, 8)
        writer.bits(LEVEL_4_0, 8)
        writer.unsignedExpGolomb(0)
        highProfileFields(writer)
        writer.unsignedExpGolomb(0)
        pictureOrderCount(writer)
        writer.unsignedExpGolomb(1)
        writer.bits(0, 1)
        writer.unsignedExpGolomb(1920L / MACROBLOCK_SIZE - 1)
        writer.unsignedExpGolomb(1088L / MACROBLOCK_SIZE - 1)
        writer.bits(1, 1)
        writer.bits(1, 1)
        writer.bits(1, 1)
        writer.unsignedExpGolomb(0)
        writer.unsignedExpGolomb(0)
        writer.unsignedExpGolomb(0)
        writer.unsignedExpGolomb(4)
        writer.bits(0, 1)
        return byteArrayOf(SPS_HEADER) + escape(writer.rbsp())
    }

    private fun escape(rbsp: ByteArray): ByteArray {
        val escaped = mutableListOf<Byte>()
        var zeros = 0
        for (byte in rbsp) {
            if (zeros >= 2 && (byte.toInt() and 0xFF) <= EMULATION_PREVENTION) {
                escaped.add(EMULATION_PREVENTION.toByte())
                zeros = 0
            }
            escaped.add(byte)
            zeros = if (byte.toInt() == 0) zeros + 1 else 0
        }
        return escaped.toByteArray()
    }

    private fun containsEmulationPrevention(unit: ByteArray): Boolean =
        (0 until unit.size - 2).any { unit[it].toInt() == 0 && unit[it + 1].toInt() == 0 && unit[it + 2].toInt() == 3 }

    private class BitWriter {
        private val bitValues = mutableListOf<Boolean>()

        fun bits(
            value: Long,
            count: Int,
        ) {
            for (shift in count - 1 downTo 0) {
                bitValues.add(((value shr shift) and 1L) == 1L)
            }
        }

        fun unsignedExpGolomb(value: Long) {
            val code = value + 1
            val length = Long.SIZE_BITS - java.lang.Long.numberOfLeadingZeros(code)
            bits(0, length - 1)
            bits(code, length)
        }

        fun signedExpGolomb(value: Long) {
            unsignedExpGolomb(if (value > 0) 2 * value - 1 else -2 * value)
        }

        fun rbsp(): ByteArray {
            bits(1, 1)
            while (bitValues.size % Byte.SIZE_BITS != 0) {
                bitValues.add(false)
            }
            return ByteArray(bitValues.size / Byte.SIZE_BITS) { byte ->
                var value = 0
                for (bit in 0 until Byte.SIZE_BITS) {
                    value = (value shl 1) or if (bitValues[byte * Byte.SIZE_BITS + bit]) 1 else 0
                }
                value.toByte()
            }
        }
    }

    private fun sizeOf(sps: ByteArray): VideoSize? {
        val range = NALUnitRange().apply { end = sps.size }
        return H264ParameterSets.size(sps, range)
    }

    private fun accessUnit(vararg units: ByteArray): ByteArray = units.fold(ByteArray(0)) { all, unit -> all + unit }

    private companion object {
        const val MAIN_PROFILE = 77
        const val HIGH_PROFILE = 100
        const val LEVEL_4_0 = 40L
        const val MACROBLOCK_SIZE = 16
        const val SPS_HEADER: Byte = 0x67
        const val EMULATION_PREVENTION = 3
        const val LARGE_POC_OFFSET = 1L shl 28
        val SPS_START_CODE = byteArrayOf(0, 0, 0, 1)
        val PPS_START_CODE = byteArrayOf(0, 0, 0, 1)
        val IDR_START_CODE = byteArrayOf(0, 0, 1)
        val PPS = bytes("68 EE 3C 80")
        val IDR = bytes("65 88 84 00 33")
        val NVENC_SPS_2944X1840 =
            bytes(
                "67 4D 40 34 95 90 02 E0 0E 7B 01 6A 04 04 0A 80 00 01 F4 00 00 EA 60 70 00 00 03 02 62 5A 00 01 " +
                    "45 85 77 94 47 C7 0A 92",
            )
        val VAAPI_SPS_2944X1840 =
            bytes(
                "67 64 0C 34 AC 2B 40 17 00 73 D3 50 10 10 14 00 00 03 00 04 00 00 03 01 E3 98 00 00 98 96 00 02 " +
                    "8B 0A F7 38 07 84 42 35",
            )
        val NVENC_SPS_640X360 =
            bytes(
                "67 4D 40 20 95 90 0A 02 FF 97 01 6A 04 04 0A 80 00 01 F4 00 00 EA 60 70 00 00 03 02 62 5A 00 01 " +
                    "45 85 77 94 47 C7 0A 92",
            )
        val VAAPI_SPS_640X360 =
            bytes(
                "67 64 0C 1F AC 2B 40 50 17 FC B3 50 10 10 14 00 00 03 00 04 00 00 03 01 E3 98 00 00 98 96 00 02 " +
                    "8B 0A F7 38 07 84 42 35",
            )

        fun bytes(hex: String): ByteArray = hex.split(' ').map { it.toInt(16).toByte() }.toByteArray()
    }
}
