package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameInfoTests {
    @Test
    fun valuesAreReadInTheNativeOrder() {
        val info = FrameInfo()
        longArrayOf(7, 1234, 1, 100, 110, 120, 1).copyInto(info.values)
        assertEquals(7L, info.frameId)
        assertEquals(1234, info.frameBytes)
        assertTrue(info.keyframe)
        assertEquals(100L, info.hostCaptureUs)
        assertEquals(110L, info.firstFragmentUs)
        assertEquals(120L, info.completeUs)
        assertEquals(Codec.H264, info.codec)
    }

    @Test
    fun theCodecFollowsTheProtocolValues() {
        val info = FrameInfo()
        assertEquals(Codec.JPEG, info.codec)
        info.values[FrameInfo.COUNT - 1] = 2
        assertEquals(Codec.UNSUPPORTED, info.codec)
    }

    @Test
    fun zeroMeansNotAKeyframe() {
        assertFalse(FrameInfo().keyframe)
    }
}
