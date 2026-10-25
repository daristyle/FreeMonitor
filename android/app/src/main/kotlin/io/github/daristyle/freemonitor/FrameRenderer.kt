package io.github.daristyle.freemonitor

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.os.SystemClock
import android.view.SurfaceHolder

class FrameRenderer(
    private val surfaceHolder: SurfaceHolder,
    private val receiver: FrameReceiver,
    private val onStatus: (RenderStatus) -> Unit,
) {
    private val frameBuffer = ByteArray(NativeBridge.maxFrameSize())
    private val decodeOptions = BitmapFactory.Options()
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val destination = Rect()
    private var bitmap: Bitmap? = null

    @Volatile
    private var running = false
    private var thread: Thread? = null

    fun start() {
        if (thread != null) {
            return
        }
        running = true
        thread = Thread(::renderLoop, RENDER_THREAD_NAME).also { it.start() }
    }

    fun stop() {
        running = false
        thread?.join()
        thread = null
    }

    private fun renderLoop() {
        var framesShown = 0
        var notJPEGFrames = 0
        var lastStatus = SystemClock.elapsedRealtime()
        var lastFrame: Long? = null
        while (running) {
            val length = receiver.nextFrame(frameBuffer, WAIT_TIMEOUT_MS)
            if (length > 0 && drawFrame(length)) {
                ++framesShown
                lastFrame = SystemClock.elapsedRealtime()
            } else if (length == FrameReceiver.NOT_JPEG) {
                ++notJPEGFrames
            }
            val now = SystemClock.elapsedRealtime()
            if (now - lastStatus >= STATUS_INTERVAL_MS) {
                onStatus(
                    RenderStatus(
                        framesPerSecond = framesShown * MILLISECONDS_PER_SECOND / (now - lastStatus),
                        millisecondsSinceLastFrame = lastFrame?.let { now - it },
                        notJPEGFrames = notJPEGFrames,
                        statistics = receiver.statistics(),
                    ),
                )
                framesShown = 0
                lastStatus = now
            }
        }
    }

    private fun drawFrame(length: Int): Boolean {
        val decoded = decode(length) ?: return false
        val canvas = surfaceHolder.lockHardwareCanvas() ?: return false
        try {
            canvas.drawColor(Color.BLACK)
            val fit = FitRectangle.fit(decoded.width, decoded.height, canvas.width, canvas.height)
            destination.set(fit.left, fit.top, fit.left + fit.width, fit.top + fit.height)
            canvas.drawBitmap(decoded, null, destination, paint)
        } finally {
            surfaceHolder.unlockCanvasAndPost(canvas)
        }
        return true
    }

    private fun decode(length: Int): Bitmap? {
        decodeOptions.inJustDecodeBounds = true
        decodeOptions.inBitmap = null
        BitmapFactory.decodeByteArray(frameBuffer, 0, length, decodeOptions)
        val width = decodeOptions.outWidth
        val height = decodeOptions.outHeight
        if (width <= 0 || height <= 0) {
            return null
        }
        val reusable =
            bitmap?.takeIf { it.width == width && it.height == height }
                ?: Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888).also { bitmap = it }
        decodeOptions.inJustDecodeBounds = false
        decodeOptions.inMutable = true
        decodeOptions.inBitmap = reusable
        return try {
            BitmapFactory.decodeByteArray(frameBuffer, 0, length, decodeOptions)
        } catch (error: IllegalArgumentException) {
            bitmap = null
            null
        }
    }

    private companion object {
        const val RENDER_THREAD_NAME = "FreeMonitor render"
        const val WAIT_TIMEOUT_MS = 100
        const val STATUS_INTERVAL_MS = 1000L
        const val MILLISECONDS_PER_SECOND = 1000f
    }
}
