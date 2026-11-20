package io.github.daristyle.freemonitor

import java.util.concurrent.Executor

class LatestSearch<T>(
    private val executor: Executor,
    private val deliver: (Runnable) -> Unit,
) {
    @Volatile
    private var generation = 0

    fun start(
        search: () -> T,
        onResult: (T) -> Unit,
    ) {
        val started = ++generation
        executor.execute {
            if (started != generation) {
                return@execute
            }
            val result = search()
            deliver(
                Runnable {
                    if (started == generation) {
                        onResult(result)
                    }
                },
            )
        }
    }

    fun cancel() {
        ++generation
    }
}
