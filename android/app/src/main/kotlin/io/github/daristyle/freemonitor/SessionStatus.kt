package io.github.daristyle.freemonitor

data class SessionStatus(
    val state: SessionState,
    val rejectReason: RejectReason,
    val sessionId: Long,
    val roundTripUs: Long?,
    val codec: Codec,
    val width: Int,
    val height: Int,
    val framesPerSecond: Int,
    val sessionCount: Long,
) {
    companion object {
        const val COUNT = 9
        private const val UNKNOWN_ROUND_TRIP = -1L

        fun fromArray(values: LongArray): SessionStatus {
            require(values.size == COUNT) { "Expected $COUNT session values, got ${values.size}" }
            return SessionStatus(
                state = SessionState.fromId(values[0]),
                rejectReason = RejectReason.fromId(values[1]),
                sessionId = values[2],
                roundTripUs = values[3].takeIf { it != UNKNOWN_ROUND_TRIP },
                codec = Codec.fromId(values[4]),
                width = values[5].toInt(),
                height = values[6].toInt(),
                framesPerSecond = values[7].toInt(),
                sessionCount = values[8],
            )
        }
    }
}
