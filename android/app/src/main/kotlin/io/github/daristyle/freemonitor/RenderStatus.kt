package io.github.daristyle.freemonitor

data class RenderStatus(
    val framesPerSecond: Float,
    val millisecondsSinceLastFrame: Long?,
    val notJPEGFrames: Int,
    val statistics: ReceiverStatistics,
)
