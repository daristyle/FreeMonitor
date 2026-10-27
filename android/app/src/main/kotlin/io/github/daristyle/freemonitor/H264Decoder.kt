package io.github.daristyle.freemonitor

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.io.IOException
import java.nio.ByteBuffer
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

class H264Decoder(
    private val surface: Surface,
    private val maxFrameSize: Int,
    private val timingLog: FrameTimingLog?,
    private val onVideoSize: (VideoSize) -> Unit,
) : AutoCloseable {
    private val callbackThread = HandlerThread(CALLBACK_THREAD_NAME).also { it.start() }
    private val callbackHandler = Handler(callbackThread.looper)
    private val lock = Any()
    private val freeInputs = ArrayBlockingQueue<Int>(MAX_INPUT_BUFFERS)
    private val pendingFrames = Array(PENDING_FRAME_SLOTS) { PendingFrame() }
    private val spsRange = NALUnitRange()
    private val ppsRange = NALUnitRange()
    private val shownFrames = AtomicInteger()
    private val lastShownMs = AtomicLong(NEVER_SHOWN)
    private var decoder: MediaCodec? = null
    private var activeDecoder: MediaCodec? = null
    private var sps: ByteArray? = null
    private var pps: ByteArray? = null
    private var nextInputNumber = 0L

    @Volatile
    private var failed = false

    private class PendingFrame {
        val info = FrameInfo()
        var acquireUs = 0L
        var decodeStartUs = 0L
    }

    val lastShownElapsedMs: Long?
        get() = lastShownMs.get().takeIf { it != NEVER_SHOWN }

    fun takeShownFrameCount(): Int = shownFrames.getAndSet(0)

    fun decode(
        data: ByteArray,
        length: Int,
        info: FrameInfo,
        acquireUs: Long,
    ): Boolean {
        if (failed) {
            closeDecoder()
        }
        if (info.keyframe && !configureFor(data, length)) {
            return false
        }
        val codec = decoder ?: return false
        val index = freeInputs.poll(INPUT_WAIT_MS, TimeUnit.MILLISECONDS) ?: return false
        return try {
            val input = checkNotNull(codec.getInputBuffer(index))
            if (input.capacity() < length) {
                codec.queueInputBuffer(index, 0, 0, 0, 0)
                return false
            }
            input.clear()
            input.put(data, 0, length)
            val inputNumber = nextInputNumber++
            synchronized(lock) {
                val pending = pendingFrames[(inputNumber % PENDING_FRAME_SLOTS).toInt()]
                info.values.copyInto(pending.info.values)
                pending.acquireUs = acquireUs
                pending.decodeStartUs = nowUs()
            }
            val flags = if (info.keyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
            codec.queueInputBuffer(index, 0, length, inputNumber, flags)
            true
        } catch (error: IllegalStateException) {
            Log.w(TAG, "The H.264 decoder rejected a frame", error)
            closeDecoder()
            false
        }
    }

    override fun close() {
        closeDecoder()
        callbackThread.quitSafely()
        callbackThread.join()
    }

    private fun configureFor(
        data: ByteArray,
        length: Int,
    ): Boolean {
        if (!H264ParameterSets.find(data, length, H264ParameterSets.NAL_TYPE_SPS, spsRange) ||
            !H264ParameterSets.find(data, length, H264ParameterSets.NAL_TYPE_PPS, ppsRange)
        ) {
            return decoder != null
        }
        if (decoder != null &&
            H264ParameterSets.sameUnit(data, spsRange, sps) &&
            H264ParameterSets.sameUnit(data, ppsRange, pps)
        ) {
            return true
        }
        val size = H264ParameterSets.size(data, spsRange) ?: return false
        closeDecoder()
        val newSPS = H264ParameterSets.withStartCode(data, spsRange)
        val newPPS = H264ParameterSets.withStartCode(data, ppsRange)
        val codec =
            startDecoder(size, newSPS, newPPS, latencyKeys = true)
                ?: startDecoder(size, newSPS, newPPS, latencyKeys = false)?.also {
                    Log.i(TAG, "The H.264 decoder only started without the low-latency keys")
                }
                ?: return false
        decoder = codec
        sps = newSPS
        pps = newPPS
        failed = false
        onVideoSize(size)
        return true
    }

    private fun startDecoder(
        size: VideoSize,
        sps: ByteArray,
        pps: ByteArray,
        latencyKeys: Boolean,
    ): MediaCodec? {
        val codec =
            try {
                MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            } catch (error: IOException) {
                Log.w(TAG, "There is no H.264 decoder", error)
                return null
            }
        return try {
            codec.setCallback(callback, callbackHandler)
            codec.configure(decoderFormat(size, sps, pps, latencyKeys), surface, null, 0)
            synchronized(lock) { activeDecoder = codec }
            codec.start()
            codec
        } catch (error: IllegalStateException) {
            Log.w(TAG, "Could not start the H.264 decoder for ${size.width}x${size.height}", error)
            releaseUnstarted(codec)
            null
        } catch (error: IllegalArgumentException) {
            Log.w(TAG, "The H.264 decoder does not accept ${size.width}x${size.height}", error)
            releaseUnstarted(codec)
            null
        }
    }

    private fun decoderFormat(
        size: VideoSize,
        sps: ByteArray,
        pps: ByteArray,
        latencyKeys: Boolean,
    ): MediaFormat =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size.width, size.height).apply {
            setByteBuffer(CSD_SPS, ByteBuffer.wrap(sps))
            setByteBuffer(CSD_PPS, ByteBuffer.wrap(pps))
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, maxFrameSize)
            if (latencyKeys) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                setInteger(MediaFormat.KEY_PRIORITY, REALTIME_PRIORITY)
                setInteger(MediaFormat.KEY_OPERATING_RATE, MAXIMUM_OPERATING_RATE)
            }
        }

    private fun releaseUnstarted(codec: MediaCodec) {
        synchronized(lock) {
            activeDecoder = null
            freeInputs.clear()
        }
        codec.release()
    }

    private fun closeDecoder() {
        val codec = decoder ?: return
        synchronized(lock) {
            activeDecoder = null
            freeInputs.clear()
        }
        try {
            codec.stop()
        } catch (error: IllegalStateException) {
            Log.w(TAG, "Stopping the H.264 decoder failed", error)
        }
        codec.release()
        decoder = null
        sps = null
        pps = null
        failed = false
    }

    private val callback =
        object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(
                codec: MediaCodec,
                index: Int,
            ) {
                synchronized(lock) {
                    if (codec === activeDecoder) {
                        freeInputs.offer(index)
                    }
                }
            }

            override fun onOutputBufferAvailable(
                codec: MediaCodec,
                index: Int,
                bufferInfo: MediaCodec.BufferInfo,
            ) {
                val decodeEndUs = nowUs()
                synchronized(lock) {
                    if (codec !== activeDecoder) {
                        return
                    }
                    try {
                        codec.releaseOutputBuffer(index, true)
                    } catch (error: IllegalStateException) {
                        return
                    }
                    shownFrames.incrementAndGet()
                    lastShownMs.set(SystemClock.elapsedRealtime())
                    val pending = pendingFrames[(bufferInfo.presentationTimeUs % PENDING_FRAME_SLOTS).toInt()]
                    timingLog?.record(
                        pending.info,
                        FrameTimingLog.Outcome.SHOWN,
                        pending.acquireUs,
                        pending.decodeStartUs,
                        decodeEndUs,
                        nowUs(),
                    )
                }
            }

            override fun onError(
                codec: MediaCodec,
                error: MediaCodec.CodecException,
            ) {
                Log.w(TAG, "The H.264 decoder failed", error)
                synchronized(lock) {
                    if (codec === activeDecoder) {
                        failed = true
                    }
                }
            }

            override fun onOutputFormatChanged(
                codec: MediaCodec,
                format: MediaFormat,
            ) = Unit
        }

    private fun nowUs(): Long = System.nanoTime() / NANOSECONDS_PER_MICROSECOND

    private companion object {
        const val TAG = "FreeMonitor"
        const val CALLBACK_THREAD_NAME = "FreeMonitor decoder"
        const val CSD_SPS = "csd-0"
        const val CSD_PPS = "csd-1"
        const val MAX_INPUT_BUFFERS = 64
        const val PENDING_FRAME_SLOTS = 32
        const val INPUT_WAIT_MS = 50L
        const val REALTIME_PRIORITY = 0
        const val MAXIMUM_OPERATING_RATE = Short.MAX_VALUE.toInt()
        const val NEVER_SHOWN = -1L
        const val NANOSECONDS_PER_MICROSECOND = 1000L
    }
}
