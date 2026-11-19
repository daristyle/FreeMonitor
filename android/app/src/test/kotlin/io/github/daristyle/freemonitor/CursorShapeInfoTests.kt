package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CursorShapeInfoTests {
    @Test
    fun fieldsAreReadWithTheNativeBitLayout() {
        val shape = CursorShapeInfo(0x0001_0005_0020_0018L)
        assertTrue(shape.found)
        assertEquals(24, shape.width)
        assertEquals(32, shape.height)
        assertEquals(5, shape.hotspotX)
        assertEquals(1, shape.hotspotY)
    }

    @Test
    fun theLargestShapeIsStillFound() {
        val shape = CursorShapeInfo(0x00FF_00FF_0100_0100L)
        assertTrue(shape.found)
        assertEquals(256, shape.width)
        assertEquals(256, shape.height)
        assertEquals(255, shape.hotspotX)
        assertEquals(255, shape.hotspotY)
    }

    @Test
    fun minusOneMeansNoShape() {
        assertFalse(CursorShapeInfo.NONE.found)
        assertFalse(CursorShapeInfo(-1L).found)
    }
}
