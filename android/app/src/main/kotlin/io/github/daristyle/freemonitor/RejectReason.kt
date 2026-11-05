package io.github.daristyle.freemonitor

enum class RejectReason {
    UNSUPPORTED_VERSION,
    NO_COMMON_CODEC,
    INVALID_PARAMETERS,
    UNKNOWN,
    ;

    companion object {
        private const val UNSUPPORTED_VERSION_ID = 1L
        private const val NO_COMMON_CODEC_ID = 2L
        private const val INVALID_PARAMETERS_ID = 3L

        fun fromId(id: Long): RejectReason =
            when (id) {
                UNSUPPORTED_VERSION_ID -> UNSUPPORTED_VERSION
                NO_COMMON_CODEC_ID -> NO_COMMON_CODEC
                INVALID_PARAMETERS_ID -> INVALID_PARAMETERS
                else -> UNKNOWN
            }
    }
}
