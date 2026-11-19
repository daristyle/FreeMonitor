package io.github.daristyle.freemonitor

class CursorPlacement {
    var left = 0f
        private set
    var top = 0f
        private set
    var right = 0f
        private set
    var bottom = 0f
        private set

    fun place(
        video: VideoSize,
        screen: FitRectangle,
        state: CursorState,
        shape: CursorShapeInfo,
    ) {
        val scaleX = screen.width.toFloat() / video.width
        val scaleY = screen.height.toFloat() / video.height
        left = screen.left + (state.x - shape.hotspotX) * scaleX
        top = screen.top + (state.y - shape.hotspotY) * scaleY
        right = left + shape.width * scaleX
        bottom = top + shape.height * scaleY
    }
}
