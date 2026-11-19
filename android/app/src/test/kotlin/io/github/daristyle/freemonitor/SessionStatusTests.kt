package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class SessionStatusTests {
    @Test
    fun valuesAreReadInTheNativeOrder() {
        val status = SessionStatus.fromArray(longArrayOf(2, 0, 0xDEADBEEF, 1500, 1, 2944, 1840, 60, 3))
        assertEquals(
            SessionStatus(
                state = SessionState.CONNECTED,
                rejectReason = RejectReason.UNKNOWN,
                sessionId = 0xDEADBEEF,
                roundTripUs = 1500,
                codec = Codec.H264,
                width = 2944,
                height = 1840,
                framesPerSecond = 60,
                sessionCount = 3,
            ),
            status,
        )
    }

    @Test
    fun anUnknownRoundTripIsNull() {
        assertNull(SessionStatus.fromArray(longArrayOf(1, 0, 7, -1, 0, 0, 0, 0, 1)).roundTripUs)
    }

    @Test
    fun statesMatchTheNativeNumbers() {
        assertEquals(SessionState.IDLE, SessionState.fromId(0))
        assertEquals(SessionState.CONNECTING, SessionState.fromId(1))
        assertEquals(SessionState.CONNECTED, SessionState.fromId(2))
        assertEquals(SessionState.REJECTED, SessionState.fromId(3))
        assertEquals(SessionState.ENDED_BY_HOST, SessionState.fromId(4))
        assertEquals(SessionState.IDLE, SessionState.fromId(9))
    }

    @Test
    fun rejectReasonsMatchTheNativeNumbers() {
        assertEquals(RejectReason.UNSUPPORTED_VERSION, RejectReason.fromId(1))
        assertEquals(RejectReason.NO_COMMON_CODEC, RejectReason.fromId(2))
        assertEquals(RejectReason.INVALID_PARAMETERS, RejectReason.fromId(3))
        assertEquals(RejectReason.MISSING_FEATURE, RejectReason.fromId(4))
        assertEquals(RejectReason.UNKNOWN, RejectReason.fromId(0))
        assertEquals(RejectReason.UNKNOWN, RejectReason.fromId(9))
    }

    @Test
    fun aWrongNumberOfValuesIsRejected() {
        assertThrows(IllegalArgumentException::class.java) { SessionStatus.fromArray(longArrayOf(1, 2, 3)) }
    }
}
