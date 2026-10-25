package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Test

class NativeBridgeTests {
    @Test
    fun protocolVersionComesFromTheNativeLibrary() {
        assertEquals(EXPECTED_PROTOCOL_VERSION, NativeBridge.protocolVersion())
    }

    private companion object {
        const val EXPECTED_PROTOCOL_VERSION = 1
    }
}
