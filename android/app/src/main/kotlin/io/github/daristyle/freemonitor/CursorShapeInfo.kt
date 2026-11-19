package io.github.daristyle.freemonitor

@JvmInline
value class CursorShapeInfo(
    val packed: Long,
) {
    val found: Boolean get() = packed != NONE.packed
    val width: Int get() = (packed and FIELD_MASK).toInt()
    val height: Int get() = ((packed ushr HEIGHT_SHIFT) and FIELD_MASK).toInt()
    val hotspotX: Int get() = ((packed ushr HOTSPOT_X_SHIFT) and FIELD_MASK).toInt()
    val hotspotY: Int get() = ((packed ushr HOTSPOT_Y_SHIFT) and FIELD_MASK).toInt()

    companion object {
        const val MAX_PIXELS = 256 * 256
        val NONE = CursorShapeInfo(-1L)

        private const val HEIGHT_SHIFT = 16
        private const val HOTSPOT_X_SHIFT = 32
        private const val HOTSPOT_Y_SHIFT = 48
        private const val FIELD_MASK = 0xFFFFL
    }
}
