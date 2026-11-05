package io.github.daristyle.freemonitor

class FrameReceiver private constructor(
    private var handle: Long,
    private val sessionMode: Boolean,
) : AutoCloseable {
    fun nextFrame(
        destination: ByteArray,
        frameInfo: FrameInfo,
        timeoutMs: Int,
    ): Int {
        check(handle != 0L) { "FrameReceiver is closed" }
        return NativeBridge.nextFrame(handle, destination, frameInfo.values, timeoutMs)
    }

    fun statistics(): ReceiverStatistics {
        check(handle != 0L) { "FrameReceiver is closed" }
        return ReceiverStatistics.fromArray(NativeBridge.receiverStatistics(handle))
    }

    fun sessionStatus(): SessionStatus? {
        check(handle != 0L) { "FrameReceiver is closed" }
        return if (sessionMode) SessionStatus.fromArray(NativeBridge.sessionStatus(handle)) else null
    }

    fun requestKeyframe() {
        if (handle != 0L) {
            NativeBridge.requestKeyframe(handle)
        }
    }

    fun reconnect() {
        if (handle != 0L) {
            NativeBridge.reconnect(handle)
        }
    }

    override fun close() {
        if (handle != 0L) {
            NativeBridge.destroyReceiver(handle)
            handle = 0L
        }
    }

    companion object {
        fun development(port: Int): FrameReceiver =
            FrameReceiver(NativeBridge.createReceiver(port), sessionMode = false)

        fun session(
            host: String,
            width: Int,
            height: Int,
            refreshHz: Int,
        ): FrameReceiver =
            FrameReceiver(NativeBridge.createSessionReceiver(host, width, height, refreshHz), sessionMode = true)
    }
}
