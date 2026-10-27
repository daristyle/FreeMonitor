package io.github.daristyle.freemonitor

import android.graphics.ImageFormat
import android.hardware.HardwareBuffer
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Size
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

class H264DecoderTests {
    @Test
    fun hostStyleFramesAreDecodedOntoTheSurfaceAndReportTheirSize() {
        val stream = H264TestStream.encode(listOf(Size(WIDTH, HEIGHT)), FRAME_RATE, FRAME_COUNT)
        val images = AtomicInteger()
        val reportedSize = AtomicReference<VideoSize>()
        var framesShown = 0
        withDecoder(stream, images, reportedSize) { decoder ->
            val start = SystemClock.elapsedRealtime()
            stream.frames.forEachIndexed { index, frame ->
                waitFor { SystemClock.elapsedRealtime() >= start + index * MILLISECONDS_PER_SECOND / stream.frameRate }
                val data = if (index == 0) stream.codecConfig + frame else frame
                assertTrue("frame $index", decoder.decode(data, data.size, frameInfo(index, data.size, index == 0), 0))
            }
            waitFor {
                framesShown += decoder.takeShownFrameCount()
                framesShown >= stream.frames.size
            }
        }
        assertEquals(stream.frames.size, framesShown)
        assertTrue(images.get() > 0)
        assertEquals(VideoSize(stream.width, stream.height), reportedSize.get())
    }

    @Test
    fun aDeltaFrameBeforeTheFirstKeyframeIsRefused() {
        val stream = H264TestStream.encode(listOf(Size(WIDTH, HEIGHT)), FRAME_RATE, FRAME_COUNT)
        withDecoder(stream, AtomicInteger(), AtomicReference()) { decoder ->
            val delta = stream.frames[1]
            assertFalse(decoder.decode(delta, delta.size, frameInfo(1, delta.size, false), 0))
        }
    }

    private fun withDecoder(
        stream: H264TestStream,
        images: AtomicInteger,
        reportedSize: AtomicReference<VideoSize>,
        test: (H264Decoder) -> Unit,
    ) {
        val listenerThread = HandlerThread(LISTENER_THREAD_NAME).also { it.start() }
        try {
            ImageReader
                .newInstance(
                    stream.width,
                    stream.height,
                    ImageFormat.PRIVATE,
                    MAX_IMAGES,
                    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE,
                ).use { imageReader ->
                    imageReader.setOnImageAvailableListener({ reader ->
                        reader.acquireNextImage()?.close()
                        images.incrementAndGet()
                    }, Handler(listenerThread.looper))
                    H264Decoder(
                        imageReader.surface,
                        NativeBridge.maxFrameSize(),
                        null,
                    ) { reportedSize.set(it) }.use(test)
                }
        } finally {
            listenerThread.quitSafely()
        }
    }

    private fun frameInfo(
        frameId: Int,
        frameBytes: Int,
        keyframe: Boolean,
    ): FrameInfo =
        FrameInfo().apply {
            values[FRAME_ID] = frameId.toLong()
            values[FRAME_BYTES] = frameBytes.toLong()
            values[KEYFRAME] = if (keyframe) 1 else 0
            values[CODEC] = 1
        }

    private fun waitFor(condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + WAIT_TIMEOUT_MS
        while (!condition() && SystemClock.elapsedRealtime() < deadline) {
            Thread.sleep(POLL_INTERVAL_MS)
        }
    }

    private companion object {
        const val WIDTH = 1280
        const val HEIGHT = 720
        const val FRAME_RATE = 60
        const val FRAME_COUNT = 30
        const val MAX_IMAGES = 4
        const val WAIT_TIMEOUT_MS = 3000L
        const val POLL_INTERVAL_MS = 1L
        const val MILLISECONDS_PER_SECOND = 1000L
        const val LISTENER_THREAD_NAME = "H264DecoderTests images"
        const val FRAME_ID = 0
        const val FRAME_BYTES = 1
        const val KEYFRAME = 2
        const val CODEC = 6
    }
}
