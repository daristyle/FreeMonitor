package io.github.daristyle.freemonitor

class FrameReceiver(
    port: Int,
) : AutoCloseable {
    private var handle: Long = NativeBridge.createReceiver(port)

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

    override fun close() {
        if (handle != 0L) {
            NativeBridge.destroyReceiver(handle)
            handle = 0L
        }
    }
}
