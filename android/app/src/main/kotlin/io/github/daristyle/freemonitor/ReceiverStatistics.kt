package io.github.daristyle.freemonitor

data class ReceiverStatistics(
    val packets: Long,
    val invalidPackets: Long,
    val ignoredPackets: Long,
    val framesCompleted: Long,
    val framesDropped: Long,
    val framesReplaced: Long,
    val senderRestarts: Long,
) {
    companion object {
        const val COUNT = 7

        fun fromArray(values: LongArray): ReceiverStatistics {
            require(values.size == COUNT) { "Expected $COUNT statistics, got ${values.size}" }
            return ReceiverStatistics(
                packets = values[0],
                invalidPackets = values[1],
                ignoredPackets = values[2],
                framesCompleted = values[3],
                framesDropped = values[4],
                framesReplaced = values[5],
                senderRestarts = values[6],
            )
        }
    }
}
