package io.github.daristyle.freemonitor

class KeyframeGate {
    private var expectedFrameId = NO_FRAME
    private var waiting = true

    fun accept(
        frameId: Long,
        keyframe: Boolean,
    ): Boolean {
        val inOrder = frameId == expectedFrameId
        expectedFrameId = (frameId + 1) and FRAME_ID_MASK
        if (keyframe) {
            waiting = false
        } else if (!inOrder) {
            waiting = true
        }
        return !waiting
    }

    fun reset() {
        expectedFrameId = NO_FRAME
        waiting = true
    }

    private companion object {
        const val NO_FRAME = -1L
        const val FRAME_ID_MASK = 0xFFFF_FFFFL
    }
}
