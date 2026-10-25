package io.github.daristyle.freemonitor

import android.graphics.Color
import android.graphics.Paint
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Size
import java.io.File

class H264TestStream(
    val encoderName: String,
    val width: Int,
    val height: Int,
    val frameRate: Int,
    val codecConfig: ByteArray,
    val frames: List<ByteArray>,
) {
    val largestUnitSize: Int
        get() = maxOf(codecConfig.size, frames.maxOf { it.size })

    fun spsProfileAndLevel(): Pair<Int, Int>? {
        for (i in 0 until codecConfig.size - SPS_LEVEL_OFFSET) {
            val isStartCode =
                codecConfig[i].toInt() == 0 && codecConfig[i + 1].toInt() == 0 && codecConfig[i + 2].toInt() == 1
            if (isStartCode && (codecConfig[i + NAL_HEADER_OFFSET].toInt() and NAL_TYPE_MASK) == NAL_TYPE_SPS) {
                val profile = codecConfig[i + SPS_PROFILE_OFFSET].toInt() and BYTE_MASK
                val level = codecConfig[i + SPS_LEVEL_OFFSET].toInt() and BYTE_MASK
                return Pair(profile, level)
            }
        }
        return null
    }

    fun writeAnnexB(file: File) {
        file.outputStream().use { output ->
            output.write(codecConfig)
            frames.forEach { output.write(it) }
        }
    }

    companion object {
        private const val BIT_RATE = 20_000_000
        private const val KEYFRAME_INTERVAL_SECONDS = 60
        private const val END_TIMEOUT_US = 1_000_000L
        private const val NANOSECONDS_PER_SECOND = 1_000_000_000L
        private const val NANOSECONDS_PER_MILLISECOND = 1_000_000L
        private const val BOX_SIZE_DIVISOR = 4f
        private const val TEXT_SIZE_DIVISOR = 10f
        private const val NAL_HEADER_OFFSET = 3
        private const val SPS_PROFILE_OFFSET = 4
        private const val SPS_LEVEL_OFFSET = 6
        private const val NAL_TYPE_MASK = 0x1F
        private const val NAL_TYPE_SPS = 7
        private const val BYTE_MASK = 0xFF

        fun encode(
            sizes: List<Size>,
            frameRate: Int,
            frameCount: Int,
        ): H264TestStream {
            val codecList = MediaCodecList(MediaCodecList.REGULAR_CODECS)
            for (size in sizes) {
                val format = encoderFormat(size, frameRate)
                val encoderName = codecList.findEncoderForFormat(format) ?: continue
                return encode(encoderName, format, frameCount)
            }
            error("No H.264 encoder supports any of $sizes")
        }

        private fun encoderFormat(
            size: Size,
            frameRate: Int,
        ): MediaFormat =
            MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size.width, size.height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, BIT_RATE)
                setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, KEYFRAME_INTERVAL_SECONDS)
                setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }

        private fun encode(
            encoderName: String,
            format: MediaFormat,
            frameCount: Int,
        ): H264TestStream {
            val width = format.getInteger(MediaFormat.KEY_WIDTH)
            val height = format.getInteger(MediaFormat.KEY_HEIGHT)
            val frameRate = format.getInteger(MediaFormat.KEY_FRAME_RATE)
            val frames = mutableListOf<ByteArray>()
            var codecConfig = ByteArray(0)
            val encoder = MediaCodec.createByCodecName(encoderName)
            try {
                encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                val inputSurface = encoder.createInputSurface()
                encoder.start()
                val info = MediaCodec.BufferInfo()

                fun drain(untilEndOfStream: Boolean) {
                    while (true) {
                        val index = encoder.dequeueOutputBuffer(info, if (untilEndOfStream) END_TIMEOUT_US else 0L)
                        if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
                            check(!untilEndOfStream) { "$encoderName did not finish the stream" }
                            return
                        }
                        if (index < 0) {
                            continue
                        }
                        val bytes = ByteArray(info.size)
                        checkNotNull(encoder.getOutputBuffer(index)).run {
                            position(info.offset)
                            get(bytes)
                        }
                        encoder.releaseOutputBuffer(index, false)
                        when {
                            (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0 -> codecConfig += bytes
                            bytes.isNotEmpty() -> frames += bytes
                        }
                        if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            return
                        }
                    }
                }

                val boxPaint = Paint().apply { color = Color.WHITE }
                val textPaint =
                    Paint().apply {
                        color = Color.YELLOW
                        textSize = height / TEXT_SIZE_DIVISOR
                    }
                val boxSize = height / BOX_SIZE_DIVISOR
                val frameIntervalNs = NANOSECONDS_PER_SECOND / frameRate
                val start = System.nanoTime()
                try {
                    for (frame in 0 until frameCount) {
                        val canvas = inputSurface.lockHardwareCanvas()
                        try {
                            canvas.drawColor(Color.DKGRAY)
                            val left = (width - boxSize) * frame / (frameCount - 1)
                            val top = (height - boxSize) / 2
                            canvas.drawRect(left, top, left + boxSize, top + boxSize, boxPaint)
                            canvas.drawText(frame.toString(), boxSize / 4, height - boxSize / 4, textPaint)
                        } finally {
                            inputSurface.unlockCanvasAndPost(canvas)
                        }
                        drain(untilEndOfStream = false)
                        val sleepMs =
                            (start + (frame + 1) * frameIntervalNs - System.nanoTime()) / NANOSECONDS_PER_MILLISECOND
                        if (sleepMs > 0) {
                            Thread.sleep(sleepMs)
                        }
                    }
                    encoder.signalEndOfInputStream()
                    drain(untilEndOfStream = true)
                } finally {
                    inputSurface.release()
                }
                if (codecConfig.isEmpty()) {
                    codecConfig = codecSpecificData(encoder.outputFormat)
                }
            } finally {
                encoder.release()
            }
            check(codecConfig.isNotEmpty() && frames.isNotEmpty()) { "$encoderName produced no stream" }
            return H264TestStream(encoderName, width, height, frameRate, codecConfig, frames)
        }

        private fun codecSpecificData(format: MediaFormat): ByteArray =
            listOf("csd-0", "csd-1")
                .mapNotNull { format.getByteBuffer(it) }
                .fold(ByteArray(0)) { all, buffer ->
                    all +
                        ByteArray(buffer.remaining()).also { buffer.duplicate().get(it) }
                }
    }
}
