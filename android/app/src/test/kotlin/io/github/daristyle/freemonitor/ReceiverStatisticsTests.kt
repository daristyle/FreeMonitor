package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class ReceiverStatisticsTests {
    @Test
    fun valuesAreReadInTheNativeOrder() {
        val statistics = ReceiverStatistics.fromArray(longArrayOf(1, 2, 3, 4, 5, 6, 7))
        assertEquals(ReceiverStatistics(1, 2, 3, 4, 5, 6, 7), statistics)
    }

    @Test
    fun aWrongNumberOfValuesIsRejected() {
        assertThrows(IllegalArgumentException::class.java) { ReceiverStatistics.fromArray(longArrayOf(1, 2, 3)) }
    }
}
