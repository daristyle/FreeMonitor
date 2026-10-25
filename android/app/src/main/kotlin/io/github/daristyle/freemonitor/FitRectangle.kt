package io.github.daristyle.freemonitor

data class FitRectangle(
    val left: Int,
    val top: Int,
    val width: Int,
    val height: Int,
) {
    companion object {
        fun fit(
            sourceWidth: Int,
            sourceHeight: Int,
            targetWidth: Int,
            targetHeight: Int,
        ): FitRectangle {
            if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
                return FitRectangle(0, 0, 0, 0)
            }
            val sourceIsWider = sourceWidth.toLong() * targetHeight >= sourceHeight.toLong() * targetWidth
            val width: Int
            val height: Int
            if (sourceIsWider) {
                width = targetWidth
                height = ((sourceHeight.toLong() * targetWidth + sourceWidth / 2) / sourceWidth).toInt()
            } else {
                height = targetHeight
                width = ((sourceWidth.toLong() * targetHeight + sourceHeight / 2) / sourceHeight).toInt()
            }
            return FitRectangle((targetWidth - width) / 2, (targetHeight - height) / 2, width, height)
        }
    }
}
