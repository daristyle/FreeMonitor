package io.github.daristyle.freemonitor

enum class SessionState {
    IDLE,
    CONNECTING,
    CONNECTED,
    REJECTED,
    ENDED_BY_HOST,
    ;

    companion object {
        private const val CONNECTING_ID = 1L
        private const val CONNECTED_ID = 2L
        private const val REJECTED_ID = 3L
        private const val ENDED_BY_HOST_ID = 4L

        fun fromId(id: Long): SessionState =
            when (id) {
                CONNECTING_ID -> CONNECTING
                CONNECTED_ID -> CONNECTED
                REJECTED_ID -> REJECTED
                ENDED_BY_HOST_ID -> ENDED_BY_HOST
                else -> IDLE
            }
    }
}
