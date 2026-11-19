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
    external fun createSessionReceiver(
        host: String,
        width: Int,
        height: Int,
        refreshHz: Int,
    ): Long

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
    external fun requestKeyframe(handle: Long)

    @JvmStatic
    external fun reconnect(handle: Long)

    @JvmStatic
    external fun sessionStatus(handle: Long): LongArray

    @JvmStatic
    external fun cursorState(handle: Long): Long

    @JvmStatic
    external fun cursorShape(
        handle: Long,
        state: Long,
        pixels: IntArray,
    ): Long

    @JvmStatic
    external fun destroyReceiver(handle: Long)
}
