package io.github.daristyle.freemonitor

class NoAnswerWatch(
    private val delayMs: Long,
) {
    private var connectingSinceMs: Long? = null
    private var fired = false

    fun reset() {
        connectingSinceMs = null
        fired = false
    }

    fun shouldSearch(
        state: SessionState,
        nowMs: Long,
    ): Boolean {
        if (state == SessionState.CONNECTED) {
            reset()
            return false
        }
        if (state != SessionState.CONNECTING) {
            connectingSinceMs = null
            return false
        }
        val since = connectingSinceMs ?: nowMs.also { connectingSinceMs = it }
        if (fired || nowMs - since < delayMs) {
            return false
        }
        fired = true
        return true
    }
}
