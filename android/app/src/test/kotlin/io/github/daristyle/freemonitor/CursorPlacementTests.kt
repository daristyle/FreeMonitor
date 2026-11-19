package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Test

class CursorPlacementTests {
    @Test
    fun sameSizeVideoPlacesTheHotspotOnThePosition() {
        val placement = place(VideoSize(2944, 1840), 2944, 1840, x = 100, y = 50)
        assertEquals(95f, placement.left, DELTA)
        assertEquals(49f, placement.top, DELTA)
        assertEquals(119f, placement.right, DELTA)
        assertEquals(73f, placement.bottom, DELTA)
    }

    @Test
    fun letterboxedVideoMovesAndScalesTheCursorWithTheFrame() {
        val placement = place(VideoSize(1920, 1080), 2944, 1840, x = 960, y = 540)
        val scale = 2944f / 1920
        assertEquals((960 - 5) * scale, placement.left, DELTA)
        assertEquals(92 + (540 - 1) * scale, placement.top, DELTA)
        assertEquals(placement.left + 24 * scale, placement.right, DELTA)
        assertEquals(placement.top + 24 * scale, placement.bottom, DELTA)
    }

    @Test
    fun aHotspotLeftOfTheFrameGivesANegativeLeftEdge() {
        val placement = place(VideoSize(2944, 1840), 2944, 1840, x = -15, y = 0)
        assertEquals(-20f, placement.left, DELTA)
        assertEquals(-1f, placement.top, DELTA)
    }

    private fun place(
        video: VideoSize,
        viewWidth: Int,
        viewHeight: Int,
        x: Int,
        y: Int,
    ): CursorPlacement {
        val screen = FitRectangle.fit(video.width, video.height, viewWidth, viewHeight)
        val state = CursorState((x.toLong() and 0xFFFF) or ((y.toLong() and 0xFFFF) shl 16))
        return CursorPlacement().apply { place(video, screen, state, ARROW) }
    }

    private companion object {
        const val DELTA = 0.001f
        val ARROW = CursorShapeInfo(0x0001_0005_0018_0018L)
    }
}
