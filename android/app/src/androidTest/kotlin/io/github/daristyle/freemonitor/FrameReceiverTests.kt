package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.net.DatagramSocket
import java.net.InetSocketAddress

class FrameReceiverTests {
    @Test
    fun anIdleReceiverReturnsNoFrameAndZeroStatistics() {
        FrameReceiver(ANY_FREE_PORT).use { receiver ->
            val frameBuffer = ByteArray(NativeBridge.maxFrameSize())
            assertEquals(0, receiver.nextFrame(frameBuffer, SHORT_TIMEOUT_MS))
            assertEquals(ReceiverStatistics(0, 0, 0, 0, 0, 0, 0), receiver.statistics())
        }
    }

    @Test
    fun closingTwiceIsHarmless() {
        val receiver = FrameReceiver(ANY_FREE_PORT)
        receiver.close()
        receiver.close()
    }

    @Test
    fun aTakenPortThrowsWithAMessage() {
        DatagramSocket(InetSocketAddress(ANY_FREE_PORT)).use { occupied ->
            val error = assertThrows(IllegalStateException::class.java) { FrameReceiver(occupied.localPort) }
            assertEquals(true, error.message?.isNotEmpty())
        }
    }

    @Test
    fun theFrameBufferHoldsTheLargestFrame() {
        assertEquals(4 * 1024 * 1024, NativeBridge.maxFrameSize())
    }

    private companion object {
        const val ANY_FREE_PORT = 0
        const val SHORT_TIMEOUT_MS = 50
    }
}
