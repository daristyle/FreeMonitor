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
    private val timingLog: FrameTimingLog?,
    private val onStatus: (RenderStatus) -> Unit,
    private val onVideoSize: (VideoSize) -> Unit,
    private val onCodecChanged: () -> Unit,
) {
    private val frameBuffer = ByteArray(NativeBridge.maxFrameSize())
    private val frameInfo = FrameInfo()
    private val decodeOptions = BitmapFactory.Options()
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val destination = Rect()
    private val keyframeGate = KeyframeGate()
    private var bitmap: Bitmap? = null
    private var codec: Codec? = null
    private var h264Decoder: H264Decoder? = null
    private var codecChangeReported = false
    private var jpegFramesShown = 0
    private var framesWaitingForKeyframe = 0
    private var lastJPEGFrameMs = NO_FRAME

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
        h264Decoder?.close()
        h264Decoder = null
    }

    private fun renderLoop() {
        var lastStatus = SystemClock.elapsedRealtime()
        while (running) {
            val length = receiver.nextFrame(frameBuffer, frameInfo, WAIT_TIMEOUT_MS)
            if (length > 0) {
                handleFrame(length, nowUs())
            }
            val now = SystemClock.elapsedRealtime()
            if (now - lastStatus >= STATUS_INTERVAL_MS) {
                val framesShown = jpegFramesShown + (h264Decoder?.takeShownFrameCount() ?: 0)
                val lastFrame = h264Decoder?.lastShownElapsedMs ?: lastJPEGFrameMs.takeIf { it != NO_FRAME }
                onStatus(
                    RenderStatus(
                        framesPerSecond = framesShown * MILLISECONDS_PER_SECOND / (now - lastStatus),
                        millisecondsSinceLastFrame = lastFrame?.let { now - it },
                        codec = codec,
                        framesWaitingForKeyframe = framesWaitingForKeyframe,
                        statistics = receiver.statistics(),
                    ),
                )
                jpegFramesShown = 0
                lastStatus = now
            }
        }
    }

    private fun handleFrame(
        length: Int,
        acquireUs: Long,
    ) {
        val frameCodec = frameInfo.codec
        if (frameCodec == Codec.UNSUPPORTED) {
            timingLog?.record(frameInfo, FrameTimingLog.Outcome.UNSUPPORTED_CODEC, acquireUs, 0, 0, 0)
            return
        }
        val surfaceCodec = codec ?: frameCodec.also { codec = it }
        if (frameCodec != surfaceCodec) {
            if (!codecChangeReported) {
                codecChangeReported = true
                onCodecChanged()
            }
            timingLog?.record(frameInfo, FrameTimingLog.Outcome.NOT_DRAWN, acquireUs, 0, 0, 0)
            return
        }
        if (frameCodec == Codec.JPEG) {
            if (showFrame(length, acquireUs)) {
                ++jpegFramesShown
                lastJPEGFrameMs = SystemClock.elapsedRealtime()
            }
        } else {
            decodeH264(length, acquireUs)
        }
    }

    private fun decodeH264(
        length: Int,
        acquireUs: Long,
    ) {
        if (!keyframeGate.accept(frameInfo.frameId, frameInfo.keyframe)) {
            ++framesWaitingForKeyframe
            timingLog?.record(frameInfo, FrameTimingLog.Outcome.WAITING_FOR_KEYFRAME, acquireUs, 0, 0, 0)
            return
        }
        val decoder =
            h264Decoder
                ?: H264Decoder(surfaceHolder.surface, frameBuffer.size, timingLog, onVideoSize).also {
                    h264Decoder = it
                }
        if (!decoder.decode(frameBuffer, length, frameInfo, acquireUs)) {
            keyframeGate.reset()
            timingLog?.record(frameInfo, FrameTimingLog.Outcome.DECODE_FAILED, acquireUs, 0, 0, 0)
        }
    }

    private fun showFrame(
        length: Int,
        acquireUs: Long,
    ): Boolean {
        val decodeStartUs = nowUs()
        val decoded = decode(length)
        val decodeEndUs = nowUs()
        val posted = decoded != null && drawFrame(decoded)
        val postUs = if (posted) nowUs() else 0L
        val outcome =
            when {
                decoded == null -> FrameTimingLog.Outcome.DECODE_FAILED
                posted -> FrameTimingLog.Outcome.SHOWN
                else -> FrameTimingLog.Outcome.NOT_DRAWN
            }
        timingLog?.record(frameInfo, outcome, acquireUs, decodeStartUs, decodeEndUs, postUs)
        return posted
    }

    private fun drawFrame(decoded: Bitmap): Boolean {
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

    private fun nowUs(): Long = System.nanoTime() / NANOSECONDS_PER_MICROSECOND

    private companion object {
        const val RENDER_THREAD_NAME = "FreeMonitor render"
        const val NO_FRAME = -1L
        const val WAIT_TIMEOUT_MS = 100
        const val STATUS_INTERVAL_MS = 1000L
        const val MILLISECONDS_PER_SECOND = 1000f
        const val NANOSECONDS_PER_MICROSECOND = 1000L
    }
}
