package io.github.daristyle.freemonitor

enum class RejectReason {
    UNSUPPORTED_VERSION,
    NO_COMMON_CODEC,
    INVALID_PARAMETERS,
    MISSING_FEATURE,
    UNKNOWN,
    ;

    companion object {
        private const val UNSUPPORTED_VERSION_ID = 1L
        private const val NO_COMMON_CODEC_ID = 2L
        private const val INVALID_PARAMETERS_ID = 3L
        private const val MISSING_FEATURE_ID = 4L

        fun fromId(id: Long): RejectReason =
            when (id) {
                UNSUPPORTED_VERSION_ID -> UNSUPPORTED_VERSION
                NO_COMMON_CODEC_ID -> NO_COMMON_CODEC
                INVALID_PARAMETERS_ID -> INVALID_PARAMETERS
                MISSING_FEATURE_ID -> MISSING_FEATURE
                else -> UNKNOWN
            }
    }
}
