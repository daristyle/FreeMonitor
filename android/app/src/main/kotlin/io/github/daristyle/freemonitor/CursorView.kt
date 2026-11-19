package io.github.daristyle.freemonitor

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.Choreographer
import android.view.View

class CursorView(
    context: Context,
) : View(context),
    Choreographer.FrameCallback {
    private val pixels = IntArray(CursorShapeInfo.MAX_PIXELS)
    private val bitmaps = arrayOfNulls<Bitmap>(CursorState.SLOT_LIMIT)
    private val shapes = LongArray(CursorState.SLOT_LIMIT) { CursorShapeInfo.NONE.packed }
    private val generations = IntArray(CursorState.SLOT_LIMIT) { NO_GENERATION }
    private val placement = CursorPlacement()
    private val target = RectF()
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private var receiver: FrameReceiver? = null
    private var videoSize: VideoSize? = null
    private var screen = FitRectangle(0, 0, 0, 0)
    private var state = CursorState.EMPTY
    private var shownSlot = CursorState.NO_SLOT

    fun start(newReceiver: FrameReceiver) {
        stop()
        receiver = newReceiver
        Choreographer.getInstance().postFrameCallback(this)
    }

    fun stop() {
        Choreographer.getInstance().removeFrameCallback(this)
        receiver = null
        bitmaps.fill(null)
        shapes.fill(CursorShapeInfo.NONE.packed)
        generations.fill(NO_GENERATION)
        state = CursorState.EMPTY
        shownSlot = CursorState.NO_SLOT
        invalidate()
    }

    fun setVideoSize(size: VideoSize) {
        if (size != videoSize) {
            videoSize = size
            updateScreen()
        }
    }

    override fun doFrame(frameTimeNanos: Long) {
        val current = receiver ?: return
        Choreographer.getInstance().postFrameCallback(this)
        val newState = current.cursorState()
        var changed = newState != state
        state = newState
        if (newState.hasShape && generations[newState.slot] != newState.generation) {
            changed = loadShape(current, newState) || changed
        }
        val newShownSlot =
            when {
                !newState.hasShape -> CursorState.NO_SLOT
                generations[newState.slot] == newState.generation -> newState.slot
                else -> shownSlot
            }
        if (newShownSlot != shownSlot) {
            shownSlot = newShownSlot
            changed = true
        }
        if (changed) {
            invalidate()
        }
    }

    override fun onSizeChanged(
        width: Int,
        height: Int,
        oldWidth: Int,
        oldHeight: Int,
    ) {
        updateScreen()
    }

    override fun onDraw(canvas: Canvas) {
        val video = videoSize ?: return
        if (!state.visible || shownSlot == CursorState.NO_SLOT || screen.width == 0 || screen.height == 0) {
            return
        }
        val bitmap = bitmaps[shownSlot] ?: return
        placement.place(video, screen, state, CursorShapeInfo(shapes[shownSlot]))
        target.set(placement.left, placement.top, placement.right, placement.bottom)
        val saved = canvas.save()
        canvas.clipRect(screen.left, screen.top, screen.left + screen.width, screen.top + screen.height)
        canvas.drawBitmap(bitmap, null, target, paint)
        canvas.restoreToCount(saved)
    }

    private fun loadShape(
        current: FrameReceiver,
        wanted: CursorState,
    ): Boolean {
        val shape = current.cursorShape(wanted, pixels)
        if (!shape.found) {
            return false
        }
        bitmaps[wanted.slot] =
            Bitmap.createBitmap(pixels, 0, shape.width, shape.width, shape.height, Bitmap.Config.ARGB_8888)
        shapes[wanted.slot] = shape.packed
        generations[wanted.slot] = wanted.generation
        return true
    }

    private fun updateScreen() {
        val video = videoSize ?: return
        screen = FitRectangle.fit(video.width, video.height, width, height)
        invalidate()
    }

    private companion object {
        const val NO_GENERATION = -1
    }
}
