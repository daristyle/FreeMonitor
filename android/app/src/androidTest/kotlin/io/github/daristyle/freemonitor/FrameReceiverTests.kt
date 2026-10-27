package io.github.daristyle.freemonitor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.nio.ByteBuffer

class FrameReceiverTests {
    @Test
    fun anIdleReceiverReturnsNoFrameAndZeroStatistics() {
        FrameReceiver(ANY_FREE_PORT).use { receiver ->
            val frameBuffer = ByteArray(NativeBridge.maxFrameSize())
            assertEquals(0, receiver.nextFrame(frameBuffer, FrameInfo(), SHORT_TIMEOUT_MS))
            assertEquals(ReceiverStatistics(0, 0, 0, 0, 0, 0, 0), receiver.statistics())
        }
    }

    @Test
    fun aFrameSentOverLoopbackComesOutWithItsInfo() {
        receiveOneFrame(JPEG, Codec.JPEG)
    }

    @Test
    fun anH264FrameComesOutWithItsDataAndCodec() {
        receiveOneFrame(H264, Codec.H264)
    }

    private fun receiveOneFrame(
        codecId: Byte,
        codec: Codec,
    ) {
        val port = DatagramSocket(ANY_FREE_PORT).use { it.localPort }
        FrameReceiver(port).use { receiver ->
            val payload = ByteArray(PAYLOAD_SIZE) { (it * 7).toByte() }
            val sentUs = nowUs()
            DatagramSocket().use { sender ->
                val packet = videoFragment(FRAME_ID, CAPTURE_US, sentUs, codecId, payload)
                sender.send(DatagramPacket(packet, packet.size, InetAddress.getByName(IPV4_LOOPBACK), port))
            }

            val frameBuffer = ByteArray(NativeBridge.maxFrameSize())
            val info = FrameInfo()
            val length = receiver.nextFrame(frameBuffer, info, LONG_TIMEOUT_MS)
            val acquiredUs = nowUs()

            assertEquals(PAYLOAD_SIZE, length)
            assertArrayEquals(payload, frameBuffer.copyOf(length))
            assertEquals(FRAME_ID.toLong(), info.frameId)
            assertEquals(PAYLOAD_SIZE, info.frameBytes)
            assertTrue(info.keyframe)
            assertEquals(CAPTURE_US, info.hostCaptureUs)
            assertTrue(info.firstFragmentUs in sentUs..info.completeUs)
            assertTrue(info.completeUs <= acquiredUs)
            assertEquals(codec, info.codec)
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

    private fun videoFragment(
        frameId: Int,
        captureUs: Long,
        timestampUs: Long,
        codecId: Byte,
        payload: ByteArray,
    ): ByteArray =
        ByteBuffer
            .allocate(FRAGMENT_HEADER_SIZE + payload.size)
            .putShort(MAGIC)
            .put(PROTOCOL_VERSION)
            .put(VIDEO_FRAGMENT)
            .putInt(DEVELOPMENT_SESSION_ID)
            .putInt(FIRST_SEQ)
            .putInt(0)
            .putLong(timestampUs)
            .putInt(frameId)
            .putShort(0)
            .putShort(1)
            .putInt(payload.size)
            .put(codecId)
            .put(KEYFRAME_FLAG)
            .putShort(0)
            .putLong(captureUs)
            .put(payload)
            .array()

    private fun nowUs(): Long = System.nanoTime() / NANOSECONDS_PER_MICROSECOND

    private companion object {
        const val ANY_FREE_PORT = 0
        const val IPV4_LOOPBACK = "127.0.0.1"
        const val SHORT_TIMEOUT_MS = 50
        const val LONG_TIMEOUT_MS = 2000
        const val PAYLOAD_SIZE = 100
        const val FRAME_ID = 0x1234_5678
        const val CAPTURE_US = 42_000_000L
        const val FRAGMENT_HEADER_SIZE = 48
        const val MAGIC: Short = 0x464D
        const val PROTOCOL_VERSION: Byte = 1
        const val VIDEO_FRAGMENT: Byte = 0x01
        const val DEVELOPMENT_SESSION_ID = 0
        const val FIRST_SEQ = 1
        const val JPEG: Byte = 0
        const val H264: Byte = 1
        const val KEYFRAME_FLAG: Byte = 1
        const val NANOSECONDS_PER_MICROSECOND = 1000L
    }
}
