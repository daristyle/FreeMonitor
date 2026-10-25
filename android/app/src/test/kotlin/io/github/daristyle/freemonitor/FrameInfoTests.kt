package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameInfoTests {
    @Test
    fun valuesAreReadInTheNativeOrder() {
        val info = FrameInfo()
        longArrayOf(7, 1234, 1, 100, 110, 120).copyInto(info.values)
        assertEquals(7L, info.frameId)
        assertEquals(1234, info.frameBytes)
        assertTrue(info.keyframe)
        assertEquals(100L, info.hostCaptureUs)
        assertEquals(110L, info.firstFragmentUs)
        assertEquals(120L, info.completeUs)
    }

    @Test
    fun zeroMeansNotAKeyframe() {
        assertFalse(FrameInfo().keyframe)
    }
}
