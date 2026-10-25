package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class NativeBridgeTests {
    @Test
    fun protocolVersionComesFromTheNativeLibrary() {
        assertEquals(EXPECTED_PROTOCOL_VERSION, NativeBridge.protocolVersion())
    }

    @Test
    fun aTooShortFrameInfoArrayIsRejected() {
        val handle = NativeBridge.createReceiver(ANY_FREE_PORT)
        try {
            assertThrows(IllegalStateException::class.java) {
                NativeBridge.nextFrame(handle, ByteArray(1), LongArray(FrameInfo.COUNT - 1), 0)
            }
        } finally {
            NativeBridge.destroyReceiver(handle)
        }
    }

    private companion object {
        const val EXPECTED_PROTOCOL_VERSION = 1
        const val ANY_FREE_PORT = 0
    }
}
