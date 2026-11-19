package io.github.daristyle.freemonitor

@JvmInline
value class CursorState(
    val packed: Long,
) {
    val x: Int get() = packed.toShort().toInt()
    val y: Int get() = (packed ushr Y_SHIFT).toShort().toInt()
    val slot: Int get() = ((packed ushr SLOT_SHIFT) and SLOT_MASK).toInt()
    val visible: Boolean get() = ((packed ushr VISIBLE_SHIFT) and 1L) != 0L
    val generation: Int get() = ((packed ushr GENERATION_SHIFT) and GENERATION_MASK).toInt()
    val hasShape: Boolean get() = slot != NO_SLOT

    companion object {
        const val NO_SLOT = 0xFF
        const val SLOT_LIMIT = NO_SLOT
        val EMPTY = CursorState(NO_SLOT.toLong() shl SLOT_SHIFT)

        private const val Y_SHIFT = 16
        private const val SLOT_SHIFT = 32
        private const val VISIBLE_SHIFT = 40
        private const val GENERATION_SHIFT = 48
        private const val SLOT_MASK = 0xFFL
        private const val GENERATION_MASK = 0xFFFFL
    }
}
