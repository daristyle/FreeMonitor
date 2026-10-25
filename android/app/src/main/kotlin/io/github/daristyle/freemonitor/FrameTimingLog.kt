package io.github.daristyle.freemonitor

import java.io.File
import java.io.IOException
import java.io.Writer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

class FrameTimingLog(
    file: File,
    private val flushIntervalMs: Long = FLUSH_INTERVAL_MS,
) : AutoCloseable {
    private val writer: Writer = file.bufferedWriter()
    private val records = LongArray(CAPACITY * FIELD_COUNT)
    private val head = AtomicLong()
    private val tail = AtomicLong()
    private val droppedRecords = AtomicLong()
    private val stopSignal = CountDownLatch(1)
    private val writerThread = Thread(::writeLoop, WRITER_THREAD_NAME)
    private var closed = false

    @Volatile
    private var failure: IOException? = null

    enum class Outcome {
        SHOWN,
        NOT_DRAWN,
        DECODE_FAILED,
        NOT_JPEG,
    }

    val droppedRecordCount: Long
        get() = droppedRecords.get()

    val failed: Boolean
        get() = failure != null

    init {
        try {
            writer.write(HEADER)
            writer.flush()
        } catch (error: IOException) {
            writer.close()
            throw error
        }
        writerThread.start()
    }

    fun record(
        info: FrameInfo,
        outcome: Outcome,
        acquireUs: Long,
        decodeStartUs: Long,
        decodeEndUs: Long,
        postUs: Long,
    ) {
        val writeIndex = head.get()
        if (writeIndex - tail.get() == CAPACITY.toLong()) {
            droppedRecords.incrementAndGet()
            return
        }
        val base = (writeIndex % CAPACITY).toInt() * FIELD_COUNT
        records[base + FRAME_ID] = info.frameId
        records[base + OUTCOME] = outcome.ordinal.toLong()
        records[base + KEYFRAME] = if (info.keyframe) 1L else 0L
        records[base + FRAME_BYTES] = info.frameBytes.toLong()
        records[base + HOST_CAPTURE_US] = info.hostCaptureUs
        records[base + FIRST_FRAGMENT_US] = info.firstFragmentUs
        records[base + COMPLETE_US] = info.completeUs
        records[base + ACQUIRE_US] = acquireUs
        records[base + DECODE_START_US] = decodeStartUs
        records[base + DECODE_END_US] = decodeEndUs
        records[base + POST_US] = postUs
        head.set(writeIndex + 1)
    }

    override fun close() {
        if (closed) {
            return
        }
        closed = true
        stopSignal.countDown()
        writerThread.join()
        try {
            writer.close()
        } catch (error: IOException) {
            failure = failure ?: error
        }
    }

    private fun writeLoop() {
        while (!stopSignal.await(flushIntervalMs, TimeUnit.MILLISECONDS)) {
            drain()
        }
        drain()
    }

    private fun drain() {
        val end = head.get()
        var index = tail.get()
        if (index == end) {
            return
        }
        val text = StringBuilder()
        while (index != end) {
            appendRow(text, (index % CAPACITY).toInt() * FIELD_COUNT)
            ++index
        }
        tail.set(end)
        if (failure != null) {
            return
        }
        try {
            writer.write(text.toString())
            writer.flush()
        } catch (error: IOException) {
            failure = error
        }
    }

    private fun appendRow(
        text: StringBuilder,
        base: Int,
    ) {
        text.append(records[base + FRAME_ID]).append(',')
        text.append(Outcome.entries[records[base + OUTCOME].toInt()].name).append(',')
        text.append(records[base + KEYFRAME]).append(',')
        text.append(records[base + FRAME_BYTES])
        for (field in HOST_CAPTURE_US..POST_US) {
            text.append(',')
            val timestampUs = records[base + field]
            if (timestampUs != 0L) {
                text.append(timestampUs)
            }
        }
        text.append('\n')
    }

    companion object {
        const val CAPACITY = 1024
        const val FLUSH_INTERVAL_MS = 500L
        private const val WRITER_THREAD_NAME = "FreeMonitor timing log"
        private const val HEADER =
            "# freemonitor-timing 1 tablet\n" +
                "frameId,outcome,keyframe,frameBytes,hostCaptureUs,firstFragmentUs,completeUs,acquireUs," +
                "decodeStartUs,decodeEndUs,postUs\n"
        private const val FRAME_ID = 0
        private const val OUTCOME = 1
        private const val KEYFRAME = 2
        private const val FRAME_BYTES = 3
        private const val HOST_CAPTURE_US = 4
        private const val FIRST_FRAGMENT_US = 5
        private const val COMPLETE_US = 6
        private const val ACQUIRE_US = 7
        private const val DECODE_START_US = 8
        private const val DECODE_END_US = 9
        private const val POST_US = 10
        private const val FIELD_COUNT = 11
    }
}
