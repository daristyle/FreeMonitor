package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Test
import java.util.concurrent.Executor

class LatestSearchTests {
    private val tasks = ArrayDeque<Runnable>()
    private val deliveries = ArrayDeque<Runnable>()
    private val searches = LatestSearch<Int>(Executor { tasks.add(it) }) { deliveries.add(it) }
    private val results = mutableListOf<Int>()
    private var searchCount = 0

    @Test
    fun aSearchDeliversItsResult() {
        start()
        runAll(tasks)
        runAll(deliveries)
        assertEquals(listOf(1), results)
    }

    @Test
    fun queuedSearchesThatAreNoLongerTheLatestDoNotRun() {
        repeat(QUICK_TAPS) { start() }
        runAll(tasks)
        assertEquals(1, searchCount)
        runAll(deliveries)
        assertEquals(listOf(1), results)
    }

    @Test
    fun aNewerSearchDropsTheResultOfARunningOne() {
        start()
        runAll(tasks)
        start()
        runAll(deliveries)
        assertEquals(emptyList<Int>(), results)
        runAll(tasks)
        runAll(deliveries)
        assertEquals(listOf(2), results)
    }

    @Test
    fun cancelSkipsAQueuedSearchAndDropsAFinishedOne() {
        start()
        searches.cancel()
        runAll(tasks)
        assertEquals(0, searchCount)
        start()
        runAll(tasks)
        searches.cancel()
        runAll(deliveries)
        assertEquals(1, searchCount)
        assertEquals(emptyList<Int>(), results)
    }

    private fun start() {
        searches.start({ ++searchCount }) { results.add(it) }
    }

    private fun runAll(queue: ArrayDeque<Runnable>) {
        while (queue.isNotEmpty()) {
            queue.removeFirst().run()
        }
    }

    private companion object {
        const val QUICK_TAPS = 5
    }
}
