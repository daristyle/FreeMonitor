package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.nio.ByteBuffer
import kotlin.concurrent.thread

class HostSearchTests {
    @Test
    fun aSearchFindsAHostThatAnswersOverLoopback() {
        DatagramSocket(InetSocketAddress(IPV4_LOOPBACK, ANY_FREE_PORT)).use { host ->
            host.soTimeout = LONG_TIMEOUT_MS
            var found = emptyList<DiscoveredHost>()
            val search = thread { found = HostSearch.find(intArrayOf(LOOPBACK_ADDRESS), host.localPort, SEARCH_MS) }

            val buffer = ByteArray(BUFFER_SIZE)
            val received = DatagramPacket(buffer, buffer.size)
            host.receive(received)
            val discover = ByteBuffer.wrap(buffer, 0, received.length).slice()
            assertEquals(DISCOVER_SIZE, discover.limit())
            assertEquals(MAGIC, discover.getShort(0))
            assertEquals(DISCOVER, discover.get(TYPE_OFFSET))
            assertEquals(0, discover.getInt(SESSION_ID_OFFSET))

            val announce = announce(discover.getInt(NONCE_OFFSET))
            host.send(DatagramPacket(announce, announce.size, received.socketAddress))
            search.join()

            assertEquals(listOf(DiscoveredHost(IPV4_LOOPBACK, ANNOUNCED_PORT, 1, HOST_NAME)), found)
        }
    }

    @Test
    fun aSearchWithoutAnswersFindsNothing() {
        val silentPort = DatagramSocket(ANY_FREE_PORT).use { it.localPort }
        assertEquals(emptyList<DiscoveredHost>(), HostSearch.find(intArrayOf(LOOPBACK_ADDRESS), silentPort, SEARCH_MS))
    }

    @Test
    fun aSearchWithoutAddressesOrPortThrows() {
        assertThrows(IllegalStateException::class.java) {
            HostSearch.find(intArrayOf(), HostSearch.DISCOVERY_PORT, SEARCH_MS)
        }
        assertThrows(IllegalStateException::class.java) {
            HostSearch.find(intArrayOf(LOOPBACK_ADDRESS), 0, SEARCH_MS)
        }
    }

    @Test
    fun theBroadcastAddressesLeaveOutLoopback() {
        assertTrue(NativeBridge.broadcastAddresses().none { (it ushr OCTET_BITS * 3) == LOOPBACK_FIRST_OCTET })
    }

    private fun announce(nonce: Int): ByteArray {
        val name = HOST_NAME.toByteArray(Charsets.UTF_8)
        return ByteBuffer
            .allocate(ANNOUNCE_SIZE + name.size)
            .putShort(MAGIC)
            .put(PROTOCOL_VERSION)
            .put(ANNOUNCE)
            .putInt(0)
            .putInt(FIRST_SEQ)
            .putInt(0)
            .putLong(0)
            .putInt(nonce)
            .putShort(ANNOUNCED_PORT.toShort())
            .putShort(0)
            .put(name)
            .array()
    }

    private companion object {
        const val ANY_FREE_PORT = 0
        const val IPV4_LOOPBACK = "127.0.0.1"
        const val LOOPBACK_ADDRESS = 0x7F00_0001
        const val LOOPBACK_FIRST_OCTET = 127
        const val OCTET_BITS = 8
        const val LONG_TIMEOUT_MS = 2000
        const val SEARCH_MS = 500
        const val BUFFER_SIZE = 64
        const val MAGIC: Short = 0x464D
        const val PROTOCOL_VERSION: Byte = 1
        const val DISCOVER: Byte = 0x30
        const val ANNOUNCE: Byte = 0x31
        const val DISCOVER_SIZE = 28
        const val ANNOUNCE_SIZE = 32
        const val TYPE_OFFSET = 3
        const val SESSION_ID_OFFSET = 4
        const val NONCE_OFFSET = 24
        const val FIRST_SEQ = 1
        const val ANNOUNCED_PORT = 50500
        const val HOST_NAME = "Büro-PC"
    }
}
