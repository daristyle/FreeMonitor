package io.github.daristyle.freemonitor

data class RenderStatus(
    val framesPerSecond: Float,
    val millisecondsSinceLastFrame: Long?,
    val codec: Codec?,
    val framesWaitingForKeyframe: Int,
    val statistics: ReceiverStatistics,
)
