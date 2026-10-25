package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Test

class FitRectangleTests {
    @Test
    fun sameSizeFillsTheTarget() {
        assertEquals(FitRectangle(0, 0, 2944, 1840), FitRectangle.fit(2944, 1840, 2944, 1840))
    }

    @Test
    fun widerSourceGetsBarsAboveAndBelow() {
        assertEquals(FitRectangle(0, 92, 2944, 1656), FitRectangle.fit(1920, 1080, 2944, 1840))
    }

    @Test
    fun tallerSourceGetsBarsLeftAndRight() {
        assertEquals(FitRectangle(560, 0, 800, 1000), FitRectangle.fit(800, 1000, 1920, 1000))
    }

    @Test
    fun smallerSourceIsScaledUp() {
        assertEquals(FitRectangle(0, 0, 2944, 1840), FitRectangle.fit(1472, 920, 2944, 1840))
    }

    @Test
    fun emptySizesGiveAnEmptyRectangle() {
        assertEquals(FitRectangle(0, 0, 0, 0), FitRectangle.fit(0, 1080, 2944, 1840))
        assertEquals(FitRectangle(0, 0, 0, 0), FitRectangle.fit(1920, 1080, 2944, 0))
    }
}
