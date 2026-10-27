package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class KeyframeGateTests {
    @Test
    fun theFirstFrameHasToBeAKeyframe() {
        val gate = KeyframeGate()
        assertFalse(gate.accept(10, false))
        assertFalse(gate.accept(11, false))
        assertTrue(gate.accept(12, true))
        assertTrue(gate.accept(13, false))
    }

    @Test
    fun aMissingFrameStopsDeltaFramesUntilTheNextKeyframe() {
        val gate = KeyframeGate()
        assertEquals(
            listOf(true, true, false, false, true, true),
            listOf(
                gate.accept(1, true),
                gate.accept(2, false),
                gate.accept(4, false),
                gate.accept(5, false),
                gate.accept(6, true),
                gate.accept(7, false),
            ),
        )
    }

    @Test
    fun frameIdsWrapAtTwoToTheThirtySecond() {
        val gate = KeyframeGate()
        assertTrue(gate.accept(0xFFFF_FFFFL, true))
        assertTrue(gate.accept(0, false))
        assertTrue(gate.accept(1, false))
    }

    @Test
    fun anOlderOrRepeatedFrameCountsAsAGap() {
        val gate = KeyframeGate()
        assertTrue(gate.accept(5, true))
        assertFalse(gate.accept(5, false))
        assertFalse(gate.accept(6, false))
    }

    @Test
    fun resetWaitsForTheNextKeyframe() {
        val gate = KeyframeGate()
        assertTrue(gate.accept(1, true))
        gate.reset()
        assertFalse(gate.accept(2, false))
        assertTrue(gate.accept(3, true))
    }
}
