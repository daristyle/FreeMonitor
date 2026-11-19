package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CursorStateTests {
    @Test
    fun fieldsAreReadWithTheNativeBitLayout() {
        val state = CursorState(0x1234_0105_0003_FFFEL)
        assertEquals(-2, state.x)
        assertEquals(3, state.y)
        assertTrue(state.visible)
        assertEquals(5, state.slot)
        assertEquals(0x1234, state.generation)
        assertTrue(state.hasShape)
    }

    @Test
    fun theTopBitsAndNegativeCoordinatesDoNotLeakIntoEachOther() {
        val state = CursorState(0xFFFF_000F_FFF1_8000uL.toLong())
        assertEquals(-32768, state.x)
        assertEquals(-15, state.y)
        assertEquals(15, state.slot)
        assertFalse(state.visible)
        assertEquals(0xFFFF, state.generation)
    }

    @Test
    fun theEmptyStateMatchesTheNativeEmptySnapshot() {
        assertEquals(0x0000_00FF_0000_0000L, CursorState.EMPTY.packed)
        assertFalse(CursorState.EMPTY.hasShape)
        assertFalse(CursorState.EMPTY.visible)
        assertEquals(0, CursorState.EMPTY.x)
        assertEquals(0, CursorState.EMPTY.y)
    }
}
