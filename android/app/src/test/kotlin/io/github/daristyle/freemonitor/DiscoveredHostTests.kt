package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiscoveredHostTests {
    @Test
    fun theNativeInfoUnpacksIntoAddressPortAndVersion() {
        val host = DiscoveredHost.fromNative(PACKED_HOST, NAME.toByteArray(Charsets.UTF_8))
        assertEquals("192.168.1.10", host.address)
        assertEquals(HOST_PORT, host.port)
        assertEquals(1, host.version)
        assertEquals(NAME, host.name)
        assertEquals("192.168.1.10:50500", host.endpoint)
    }

    @Test
    fun theHighestValuesStayUnsigned() {
        val host = DiscoveredHost.fromNative(ALL_BITS_SET, byteArrayOf(LETTER_A))
        assertEquals("255.255.255.255", host.address)
        assertEquals(MAX_PORT, host.port)
        assertEquals(MAX_VERSION, host.version)
    }

    @Test
    fun anInvalidUTF8NameBecomesReplacementCharacters() {
        val host = DiscoveredHost.fromNative(PACKED_HOST, byteArrayOf(LETTER_A, CUT_TWO_BYTE_CHARACTER))
        assertEquals("A�", host.name)
    }

    @Test
    fun theVersionDecidesTheCompatibility() {
        val host = DiscoveredHost("10.0.0.2", HOST_PORT, 2, NAME)
        assertEquals(HostCompatibility.COMPATIBLE, host.compatibility(2))
        assertEquals(HostCompatibility.HOST_IS_OLDER, host.compatibility(3))
        assertEquals(HostCompatibility.APP_IS_OLDER, host.compatibility(1))
    }

    @Test
    fun aHostInATetheringSubnetIsMarked() {
        val tethering = TetheringSubnets(listOf(IPv4Subnet(TETHERING_TABLET_ADDRESS, PREFIX_24)))
        val name = NAME.toByteArray(Charsets.UTF_8)
        assertTrue(DiscoveredHost.fromNative(PACKED_TETHERING_HOST, name, tethering).tethering)
        assertFalse(DiscoveredHost.fromNative(PACKED_HOST, name, tethering).tethering)
        assertFalse(DiscoveredHost.fromNative(PACKED_TETHERING_HOST, name).tethering)
    }

    @Test
    fun theListIsSortedByNameThenTetheringFirstThenAddress() {
        val wiFi = DiscoveredHost("10.121.243.165", HOST_PORT, 1, NAME)
        val cable = DiscoveredHost("10.44.224.179", HOST_PORT, 1, NAME, tethering = true)
        val other = DiscoveredHost("10.0.0.2", HOST_PORT, 1, "anna-pc")
        val secondWiFi = DiscoveredHost("10.121.243.2", HOST_PORT, 1, NAME)
        assertEquals(
            listOf(other, cable, wiFi, secondWiFi),
            listOf(wiFi, secondWiFi, cable, other).sortedWith(DiscoveredHost.LIST_ORDER),
        )
    }

    private companion object {
        const val PACKED_TETHERING_HOST = 0x0001_C544_0A2C_E0B3L
        const val TETHERING_TABLET_ADDRESS = 0x0A2C_E0B4
        const val PREFIX_24 = 24
        const val PACKED_HOST = 0x0001_C544_C0A8_010AL
        const val ALL_BITS_SET = 0x00FF_FFFF_FFFF_FFFFL
        const val HOST_PORT = 50500
        const val MAX_PORT = 65535
        const val MAX_VERSION = 255
        const val NAME = "Büro-PC"
        const val LETTER_A: Byte = 0x41
        const val CUT_TWO_BYTE_CHARACTER = 0xC3.toByte()
    }
}
