package io.github.daristyle.freemonitor

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class IPv4SubnetTests {
    @Test
    fun aSubnetContainsItsWholeRangeAndNothingNextToIt() {
        val subnet = IPv4Subnet(TETHERING_ADDRESS, PREFIX_24)
        assertTrue(subnet.contains(TETHERING_NETWORK))
        assertTrue(subnet.contains(PC_ADDRESS))
        assertTrue(subnet.contains(TETHERING_BROADCAST))
        assertFalse(subnet.contains(TETHERING_NETWORK - 1))
        assertFalse(subnet.contains(TETHERING_BROADCAST + 1))
    }

    @Test
    fun addressesWithTheHighBitSetWork() {
        val subnet = IPv4Subnet(HOME_ADDRESS, PREFIX_24)
        assertTrue(subnet.contains(HOME_BROADCAST))
        assertFalse(subnet.contains(PC_ADDRESS))
    }

    @Test
    fun aSlash32ContainsOnlyItself() {
        val subnet = IPv4Subnet(PC_ADDRESS, PREFIX_32)
        assertTrue(subnet.contains(PC_ADDRESS))
        assertFalse(subnet.contains(PC_ADDRESS + 1))
    }

    @Test
    fun aSlash0ContainsEverything() {
        val subnet = IPv4Subnet(PC_ADDRESS, 0)
        assertTrue(subnet.contains(HOME_BROADCAST))
        assertTrue(subnet.contains(0))
    }

    private companion object {
        const val PREFIX_24 = 24
        const val PREFIX_32 = 32
        const val TETHERING_NETWORK = 0x0A2C_E000
        const val TETHERING_ADDRESS = 0x0A2C_E0B4
        const val PC_ADDRESS = 0x0A2C_E0B3
        const val TETHERING_BROADCAST = 0x0A2C_E0FF
        val HOME_ADDRESS = 0xC0A8_010A.toInt()
        val HOME_BROADCAST = 0xC0A8_01FF.toInt()
    }
}
