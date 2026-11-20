package io.github.daristyle.freemonitor

import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertFalse
import org.junit.Test

class TetheringSubnetsTests {
    @Test
    fun loopbackIsNeverATetheringSubnet() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        assertFalse(TetheringSubnets.find(context).contains(LOOPBACK_ADDRESS))
    }

    private companion object {
        const val LOOPBACK_ADDRESS = 0x7F00_0001
    }
}
