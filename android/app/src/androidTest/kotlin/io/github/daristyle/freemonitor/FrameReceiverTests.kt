package io.github.daristyle.freemonitor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
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
        FrameReceiver.development(ANY_FREE_PORT).use { receiver ->
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
        FrameReceiver.development(port).use { receiver ->
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
        val receiver = FrameReceiver.development(ANY_FREE_PORT)
        receiver.close()
        receiver.close()
    }

    @Test
    fun aTakenPortThrowsWithAMessage() {
        DatagramSocket(InetSocketAddress(ANY_FREE_PORT)).use { occupied ->
            val error =
                assertThrows(IllegalStateException::class.java) { FrameReceiver.development(occupied.localPort) }
            assertEquals(true, error.message?.isNotEmpty())
        }
    }

    @Test
    fun theFrameBufferHoldsTheLargestFrame() {
        assertEquals(4 * 1024 * 1024, NativeBridge.maxFrameSize())
    }

    @Test
    fun aDevelopmentReceiverHasNoSessionStatus() {
        FrameReceiver.development(ANY_FREE_PORT).use { receiver -> assertNull(receiver.sessionStatus()) }
    }

    @Test
    fun aSessionReceiverSendsAHelloAndReportsThatItIsConnecting() {
        DatagramSocket(InetSocketAddress(IPV4_LOOPBACK, ANY_FREE_PORT)).use { host ->
            host.soTimeout = LONG_TIMEOUT_MS
            openSessionReceiver(host).use { receiver ->
                val hello = receivePacket(host)
                assertEquals(HELLO_SIZE, hello.limit())
                assertEquals(HELLO, hello.get(TYPE_OFFSET))
                assertEquals(SCREEN_WIDTH, hello.getShort(WIDTH_OFFSET).toInt() and U16_MASK)
                assertEquals(SCREEN_HEIGHT, hello.getShort(HEIGHT_OFFSET).toInt() and U16_MASK)
                assertEquals(REFRESH_HZ, hello.getShort(REFRESH_OFFSET).toInt() and U16_MASK)
                assertEquals(BOTH_CODECS, hello.get(CODECS_OFFSET))

                val status = waitForSessionStatus(receiver) { it.state == SessionState.CONNECTING }
                assertEquals(hello.getInt(SESSION_ID_OFFSET).toLong() and U32_MASK, status.sessionId)
                assertEquals(RejectReason.UNKNOWN, status.rejectReason)
                assertNull(status.roundTripUs)
                assertEquals(0, status.width)
                assertEquals(0, status.height)
                assertEquals(0, status.framesPerSecond)
                assertEquals(1L, status.sessionCount)
            }
        }
    }

    @Test
    fun closingASessionReceiverSendsThreeByes() {
        DatagramSocket(InetSocketAddress(IPV4_LOOPBACK, ANY_FREE_PORT)).use { host ->
            host.soTimeout = LONG_TIMEOUT_MS
            val receiver = openSessionReceiver(host)
            val sessionId = receivePacket(host).getInt(SESSION_ID_OFFSET)
            receiver.close()
            var byes = 0
            while (byes < BYE_COUNT) {
                val packet = receivePacket(host)
                if (packet.get(TYPE_OFFSET) == BYE) {
                    assertEquals(sessionId, packet.getInt(SESSION_ID_OFFSET))
                    ++byes
                }
            }
        }
    }

    @Test
    fun aHostThatIsNotIPv4WithAPortThrowsWithAMessage() {
        val error =
            assertThrows(IllegalStateException::class.java) {
                FrameReceiver.session("localhost", SCREEN_WIDTH, SCREEN_HEIGHT, REFRESH_HZ)
            }
        assertTrue(error.message.orEmpty().contains("IPv4:port"))
    }

    @Test
    fun aScreenSizeOfZeroThrows() {
        assertThrows(IllegalStateException::class.java) {
            FrameReceiver.session("$IPV4_LOOPBACK:$ANY_PORT_NUMBER", 0, SCREEN_HEIGHT, REFRESH_HZ)
        }
    }

    private fun openSessionReceiver(host: DatagramSocket): FrameReceiver =
        FrameReceiver.session("$IPV4_LOOPBACK:${host.localPort}", SCREEN_WIDTH, SCREEN_HEIGHT, REFRESH_HZ)

    private fun receivePacket(socket: DatagramSocket): ByteBuffer {
        val buffer = ByteArray(CONTROL_BUFFER_SIZE)
        val packet = DatagramPacket(buffer, buffer.size)
        socket.receive(packet)
        return ByteBuffer.wrap(buffer, 0, packet.length).slice()
    }

    private fun waitForSessionStatus(
        receiver: FrameReceiver,
        condition: (SessionStatus) -> Boolean,
    ): SessionStatus {
        val deadline = System.nanoTime() + LONG_TIMEOUT_MS * NANOSECONDS_PER_MILLISECOND
        while (true) {
            val status = checkNotNull(receiver.sessionStatus())
            if (condition(status) || System.nanoTime() >= deadline) {
                return status
            }
            Thread.sleep(STATUS_POLL_MS)
        }
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
        const val NANOSECONDS_PER_MILLISECOND = 1_000_000L
        const val STATUS_POLL_MS = 5L
        const val CONTROL_BUFFER_SIZE = 64
        const val SCREEN_WIDTH = 2944
        const val SCREEN_HEIGHT = 1840
        const val REFRESH_HZ = 60
        const val ANY_PORT_NUMBER = 50500
        const val HELLO: Byte = 0x20
        const val BYE: Byte = 0x27
        const val HELLO_SIZE = 40
        const val BYE_COUNT = 3
        const val TYPE_OFFSET = 3
        const val SESSION_ID_OFFSET = 4
        const val WIDTH_OFFSET = 32
        const val HEIGHT_OFFSET = 34
        const val REFRESH_OFFSET = 36
        const val CODECS_OFFSET = 38
        const val BOTH_CODECS: Byte = 3
        const val U16_MASK = 0xFFFF
        const val U32_MASK = 0xFFFF_FFFFL
    }
}
