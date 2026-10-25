package io.github.daristyle.freemonitor

class FrameInfo {
    internal val values = LongArray(COUNT)

    val frameId: Long
        get() = values[FRAME_ID]

    val frameBytes: Int
        get() = values[FRAME_BYTES].toInt()

    val keyframe: Boolean
        get() = values[KEYFRAME] != 0L

    val hostCaptureUs: Long
        get() = values[HOST_CAPTURE_US]

    val firstFragmentUs: Long
        get() = values[FIRST_FRAGMENT_US]

    val completeUs: Long
        get() = values[COMPLETE_US]

    companion object {
        const val COUNT = 6
        private const val FRAME_ID = 0
        private const val FRAME_BYTES = 1
        private const val KEYFRAME = 2
        private const val HOST_CAPTURE_US = 3
        private const val FIRST_FRAGMENT_US = 4
        private const val COMPLETE_US = 5
    }
}
