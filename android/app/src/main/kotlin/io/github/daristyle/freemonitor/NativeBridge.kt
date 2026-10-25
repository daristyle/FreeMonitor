package io.github.daristyle.freemonitor

object NativeBridge {
    init {
        System.loadLibrary("freemonitor")
    }

    @JvmStatic
    external fun protocolVersion(): Int

    @JvmStatic
    external fun maxFrameSize(): Int

    @JvmStatic
    external fun createReceiver(port: Int): Long

    @JvmStatic
    external fun nextFrame(
        handle: Long,
        destination: ByteArray,
        frameInfo: LongArray,
        timeoutMs: Int,
    ): Int

    @JvmStatic
    external fun receiverStatistics(handle: Long): LongArray

    @JvmStatic
    external fun destroyReceiver(handle: Long)
}
