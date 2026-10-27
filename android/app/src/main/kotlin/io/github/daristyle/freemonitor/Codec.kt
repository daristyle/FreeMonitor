package io.github.daristyle.freemonitor

enum class Codec(
    val label: String,
) {
    JPEG("JPEG"),
    H264("H.264"),
    UNSUPPORTED("unsupported"),
    ;

    companion object {
        private const val JPEG_ID = 0L
        private const val H264_ID = 1L

        fun fromId(id: Long): Codec =
            when (id) {
                JPEG_ID -> JPEG
                H264_ID -> H264
                else -> UNSUPPORTED
            }
    }
}
