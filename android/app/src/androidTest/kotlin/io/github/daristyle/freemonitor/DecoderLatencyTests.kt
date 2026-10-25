package io.github.daristyle.freemonitor

import android.content.Context
import android.hardware.display.DisplayManager
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Log
import android.util.Size
import android.view.Display
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.util.Locale

class DecoderLatencyTests {
    @Test
    fun everyH264DecoderDeliversAllFramesWithAndWithoutLowLatency() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val sizes = listOf(displaySize(context), Size(FALLBACK_WIDTH, FALLBACK_HEIGHT)).distinct()
        val stream = H264TestStream.encode(sizes, FRAME_RATE, FRAME_COUNT)
        val sps = stream.spsProfileAndLevel()
        log(
            "stream ${stream.width}x${stream.height} @ ${stream.frameRate} fps, ${stream.frames.size} frames, " +
                "${stream.frames.sumOf { it.size } / BYTES_PER_KILOBYTE} KB, " +
                "SPS profile_idc ${sps?.first} level_idc ${sps?.second}, encoder ${stream.encoderName}",
        )
        val file = File(checkNotNull(context.getExternalFilesDir(null)), STREAM_FILE_NAME)
        stream.writeAnnexB(file)
        log("stream saved to ${file.absolutePath}")

        val failures = mutableListOf<String>()
        for (decoder in h264Decoders()) {
            val capabilities = decoder.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
            val video = checkNotNull(capabilities.videoCapabilities)
            log(
                "${decoder.name}: ${if (decoder.isHardwareAccelerated) "hardware" else "software"}, " +
                    "low-latency feature ${yesNo(capabilities.isFeatureSupported(LOW_LATENCY_FEATURE))}, " +
                    "${stream.width}x${stream.height}@$FRAME_RATE supported " +
                    yesNo(video.areSizeAndRateSupported(stream.width, stream.height, FRAME_RATE.toDouble())),
            )
            if (!video.isSizeSupported(stream.width, stream.height)) {
                log("  skipped: size not supported")
                continue
            }
            for (lowLatency in listOf(true, false)) {
                val label = "${decoder.name} low latency ${if (lowLatency) "on" else "off"}"
                try {
                    val measurement = DecoderLatencyMeasurement(decoder.name, stream, lowLatency)
                    val lockstep = measurement.lockstep()
                    val realtime = measurement.realtime()
                    log(
                        "  low latency ${if (lowLatency) "on " else "off"}: " +
                            "lockstep held ${lockstep.framesHeld}, " +
                            "first output after ${lockstep.firstOutputAfterInputs ?: "none"} inputs | " +
                            "realtime median ${milliseconds(realtime.latencyMedianMs)}, " +
                            "p95 ${milliseconds(realtime.latencyP95Ms)}, max ${milliseconds(realtime.latencyMaxMs)}, " +
                            "held at end ${realtime.framesHeldAtEnd} | " +
                            "decoded ${lockstep.framesDecoded} and ${realtime.framesDecoded} of ${stream.frames.size}",
                    )
                    if (lockstep.framesDecoded != stream.frames.size || realtime.framesDecoded != stream.frames.size) {
                        failures += "$label lost frames"
                    }
                } catch (error: Exception) {
                    log("  $label failed: $error")
                    failures += "$label failed: $error"
                }
            }
        }
        assertTrue(failures.joinToString("; "), failures.isEmpty())
    }

    private fun h264Decoders(): List<MediaCodecInfo> =
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
            !info.isEncoder &&
                !info.isAlias &&
                info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, ignoreCase = true) } &&
                !info
                    .getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                    .isFeatureRequired(MediaCodecInfo.CodecCapabilities.FEATURE_SecurePlayback)
        }

    private fun displaySize(context: Context): Size {
        val mode = context.getSystemService(DisplayManager::class.java).getDisplay(Display.DEFAULT_DISPLAY).mode
        return Size(maxOf(mode.physicalWidth, mode.physicalHeight), minOf(mode.physicalWidth, mode.physicalHeight))
    }

    private fun yesNo(value: Boolean): String = if (value) "yes" else "no"

    private fun milliseconds(value: Double): String = "%.1f ms".format(Locale.US, value)

    private fun log(message: String) {
        Log.i(TAG, message)
    }

    private companion object {
        const val TAG = "DecoderLatency"
        const val FRAME_RATE = 60
        const val FRAME_COUNT = 120
        const val FALLBACK_WIDTH = 1920
        const val FALLBACK_HEIGHT = 1080
        const val BYTES_PER_KILOBYTE = 1024
        const val STREAM_FILE_NAME = "decoder-latency.h264"
        const val LOW_LATENCY_FEATURE = MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency
    }
}
