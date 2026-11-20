package io.github.daristyle.freemonitor

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NoAnswerWatchTests {
    @Test
    fun itFiresOnceAfterTheDelayWhileConnecting() {
        val watch = NoAnswerWatch(DELAY_MS)
        assertFalse(watch.shouldSearch(SessionState.CONNECTING, 0))
        assertFalse(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS - 1))
        assertTrue(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS))
        assertFalse(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS * 2))
    }

    @Test
    fun aConnectionStartsAFreshWatch() {
        val watch = NoAnswerWatch(DELAY_MS)
        watch.shouldSearch(SessionState.CONNECTING, 0)
        assertTrue(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS))
        assertFalse(watch.shouldSearch(SessionState.CONNECTED, DELAY_MS + 1))
        assertFalse(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS * 2))
        assertTrue(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS * 3))
    }

    @Test
    fun anotherStateRestartsTheTiming() {
        val watch = NoAnswerWatch(DELAY_MS)
        watch.shouldSearch(SessionState.CONNECTING, 0)
        assertFalse(watch.shouldSearch(SessionState.ENDED_BY_HOST, DELAY_MS - 1))
        assertFalse(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS))
        assertFalse(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS * 2 - 1))
        assertTrue(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS * 2))
    }

    @Test
    fun resetAllowsAnotherSearch() {
        val watch = NoAnswerWatch(DELAY_MS)
        watch.shouldSearch(SessionState.CONNECTING, 0)
        assertTrue(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS))
        watch.reset()
        assertFalse(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS + 1))
        assertTrue(watch.shouldSearch(SessionState.CONNECTING, DELAY_MS * 2 + 1))
    }

    private companion object {
        const val DELAY_MS = 3000L
    }
}
