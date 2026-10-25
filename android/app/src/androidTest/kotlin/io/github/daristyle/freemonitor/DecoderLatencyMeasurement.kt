package io.github.daristyle.freemonitor

import android.graphics.ImageFormat
import android.hardware.HardwareBuffer
import android.media.ImageReader
import android.media.MediaCodec
import android.media.MediaFormat
import kotlin.math.ceil

class DecoderLatencyMeasurement(
    private val decoderName: String,
    private val stream: H264TestStream,
    private val lowLatency: Boolean,
) {
    private val frameDurationUs = MICROSECONDS_PER_SECOND / stream.frameRate
    private val frameIntervalNs = NANOSECONDS_PER_SECOND / stream.frameRate
    private val info = MediaCodec.BufferInfo()

    data class LockstepResult(
        val framesHeld: Int,
        val firstOutputAfterInputs: Int?,
        val framesDecoded: Int,
    )

    data class RealtimeResult(
        val latencyMedianMs: Double,
        val latencyP95Ms: Double,
        val latencyMaxMs: Double,
        val framesHeldAtEnd: Int,
        val framesDecoded: Int,
    )

    fun lockstep(): LockstepResult =
        withDecoder { decoder ->
            var framesOut = 0
            var framesHeld = 0
            var firstOutputAfterInputs: Int? = null
            stream.frames.forEachIndexed { index, frame ->
                queue(decoder, frame, index * frameDurationUs, 0)
                val deadline = System.nanoTime() + LOCKSTEP_WAIT_NS
                while (framesOut <= index) {
                    val remainingUs = (deadline - System.nanoTime()) / NANOSECONDS_PER_MICROSECOND
                    if (remainingUs <= 0) {
                        break
                    }
                    if (dequeue(decoder, remainingUs) && isFrame()) {
                        ++framesOut
                    }
                }
                if (firstOutputAfterInputs == null && framesOut > 0) {
                    firstOutputAfterInputs = index + 1
                }
                if (index >= WARM_UP_FRAMES) {
                    framesHeld = maxOf(framesHeld, index + 1 - framesOut)
                }
            }
            framesOut += flush(decoder)
            LockstepResult(framesHeld, firstOutputAfterInputs, framesOut)
        }

    fun realtime(): RealtimeResult =
        withDecoder { decoder ->
            val queuedAt = LongArray(stream.frames.size)
            val latenciesNs = mutableListOf<Long>()
            var framesDecoded = 0

            fun pollUntil(timeNs: Long) {
                while (System.nanoTime() < timeNs) {
                    if (dequeue(decoder, POLL_TIMEOUT_US) && isFrame()) {
                        val index = (info.presentationTimeUs / frameDurationUs).toInt()
                        if (index >= WARM_UP_FRAMES) {
                            latenciesNs += System.nanoTime() - queuedAt[index]
                        }
                        ++framesDecoded
                    }
                }
            }

            val start = System.nanoTime()
            stream.frames.forEachIndexed { index, frame ->
                pollUntil(start + index * frameIntervalNs)
                queuedAt[index] = System.nanoTime()
                queue(decoder, frame, index * frameDurationUs, 0)
            }
            pollUntil(start + stream.frames.size * frameIntervalNs)
            val framesHeldAtEnd = flush(decoder)
            latenciesNs.sort()
            RealtimeResult(
                latencyMedianMs = percentileMs(latenciesNs, MEDIAN),
                latencyP95Ms = percentileMs(latenciesNs, P95),
                latencyMaxMs = percentileMs(latenciesNs, MAXIMUM),
                framesHeldAtEnd = framesHeldAtEnd,
                framesDecoded = framesDecoded + framesHeldAtEnd,
            )
        }

    private fun <T> withDecoder(measure: (MediaCodec) -> T): T =
        ImageReader
            .newInstance(
                stream.width,
                stream.height,
                ImageFormat.PRIVATE,
                MAX_IMAGES,
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE,
            ).use { imageReader ->
                val decoder = MediaCodec.createByCodecName(decoderName)
                try {
                    decoder.configure(decoderFormat(), imageReader.surface, null, 0)
                    decoder.start()
                    queue(decoder, stream.codecConfig, 0, MediaCodec.BUFFER_FLAG_CODEC_CONFIG)
                    measure(decoder)
                } finally {
                    decoder.release()
                }
            }

    private fun decoderFormat(): MediaFormat =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, stream.width, stream.height).apply {
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, stream.largestUnitSize)
            if (lowLatency) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
        }

    private fun queue(
        decoder: MediaCodec,
        data: ByteArray,
        presentationTimeUs: Long,
        flags: Int,
    ) {
        val index = decoder.dequeueInputBuffer(INPUT_TIMEOUT_US)
        check(index >= 0) { "$decoderName accepted no input for ${INPUT_TIMEOUT_US / MICROSECONDS_PER_MILLISECOND} ms" }
        checkNotNull(decoder.getInputBuffer(index)).run {
            clear()
            put(data)
        }
        decoder.queueInputBuffer(index, 0, data.size, presentationTimeUs, flags)
    }

    private fun dequeue(
        decoder: MediaCodec,
        timeoutUs: Long,
    ): Boolean {
        val index = decoder.dequeueOutputBuffer(info, timeoutUs)
        if (index < 0) {
            return false
        }
        decoder.releaseOutputBuffer(index, false)
        return true
    }

    private fun isFrame(): Boolean = info.size != 0

    private fun isEndOfStream(): Boolean = (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0

    private fun flush(decoder: MediaCodec): Int {
        queue(decoder, ByteArray(0), stream.frames.size * frameDurationUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
        var frames = 0
        val deadline = System.nanoTime() + FLUSH_TIMEOUT_NS
        while (System.nanoTime() < deadline) {
            if (dequeue(decoder, POLL_TIMEOUT_US)) {
                if (isFrame()) {
                    ++frames
                }
                if (isEndOfStream()) {
                    break
                }
            }
        }
        return frames
    }

    private fun percentileMs(
        sortedNs: List<Long>,
        fraction: Double,
    ): Double {
        if (sortedNs.isEmpty()) {
            return Double.NaN
        }
        val index = ceil(fraction * sortedNs.size).toInt().coerceIn(1, sortedNs.size) - 1
        return sortedNs[index] / NANOSECONDS_PER_MILLISECOND
    }

    private companion object {
        const val WARM_UP_FRAMES = 10
        const val MAX_IMAGES = 2
        const val LOCKSTEP_WAIT_NS = 100_000_000L
        const val FLUSH_TIMEOUT_NS = 2_000_000_000L
        const val INPUT_TIMEOUT_US = 1_000_000L
        const val POLL_TIMEOUT_US = 1_000L
        const val MICROSECONDS_PER_SECOND = 1_000_000L
        const val MICROSECONDS_PER_MILLISECOND = 1_000L
        const val NANOSECONDS_PER_SECOND = 1_000_000_000L
        const val NANOSECONDS_PER_MICROSECOND = 1_000L
        const val NANOSECONDS_PER_MILLISECOND = 1_000_000.0
        const val MEDIAN = 0.5
        const val P95 = 0.95
        const val MAXIMUM = 1.0
    }
}
