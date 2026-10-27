package io.github.daristyle.freemonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class FrameTimingLogTests {
    @get:Rule
    val folder = TemporaryFolder()

    @Test
    fun theLogStartsWithItsHeaderAndWritesOneRowPerFrame() {
        val file = folder.newFile("tablet-timing.csv")
        val log = FrameTimingLog(file, NEVER_FLUSH_MS)
        log.record(frameInfo(7, 1234, true, 100, 110, 120), FrameTimingLog.Outcome.SHOWN, 130, 131, 150, 160)
        log.record(frameInfo(8, 99, false, 200, 210, 220), FrameTimingLog.Outcome.UNSUPPORTED_CODEC, 230, 0, 0, 0)
        log.record(frameInfo(9, 98, false, 300, 310, 320), FrameTimingLog.Outcome.WAITING_FOR_KEYFRAME, 330, 0, 0, 0)
        log.close()
        log.close()

        assertEquals(
            listOf(
                "# freemonitor-timing 1 tablet",
                "frameId,outcome,keyframe,frameBytes,hostCaptureUs,firstFragmentUs,completeUs,acquireUs," +
                    "decodeStartUs,decodeEndUs,postUs",
                "7,SHOWN,1,1234,100,110,120,130,131,150,160",
                "8,UNSUPPORTED_CODEC,0,99,200,210,220,230,,,",
                "9,WAITING_FOR_KEYFRAME,0,98,300,310,320,330,,,",
            ),
            file.readLines(),
        )
        assertEquals(0L, log.droppedRecordCount)
        assertEquals(false, log.failed)
    }

    @Test
    fun aFullLogDropsRecordsInsteadOfBlocking() {
        val file = folder.newFile("tablet-timing.csv")
        FrameTimingLog(file, NEVER_FLUSH_MS).use { log ->
            repeat(FrameTimingLog.CAPACITY + 3) { frame ->
                log.record(frameInfo(frame.toLong(), 1, true, 1, 1, 1), FrameTimingLog.Outcome.SHOWN, 1, 1, 1, 1)
            }
            assertEquals(3L, log.droppedRecordCount)
        }
        val lines = file.readLines()
        assertEquals(2 + FrameTimingLog.CAPACITY, lines.size)
        assertTrue(lines[2].startsWith("0,"))
        assertTrue(lines.last().startsWith("${FrameTimingLog.CAPACITY - 1},"))
    }

    @Test
    fun twoThreadsCanRecordAtTheSameTimeWithoutLosingRecords() {
        val file = folder.newFile("tablet-timing.csv")
        val perThread = FrameTimingLog.CAPACITY / 2
        FrameTimingLog(file, NEVER_FLUSH_MS).use { log ->
            val writers =
                List(2) { writer ->
                    Thread {
                        repeat(perThread) { frame ->
                            val frameId = (writer * perThread + frame).toLong()
                            log.record(frameInfo(frameId, 1, true, 1, 1, 1), FrameTimingLog.Outcome.SHOWN, 1, 1, 1, 1)
                        }
                    }
                }
            writers.forEach(Thread::start)
            writers.forEach(Thread::join)
            assertEquals(0L, log.droppedRecordCount)
        }
        val frameIds = file.readLines().drop(2).map { it.substringBefore(',').toLong() }
        assertEquals((0L until 2L * perThread).toList(), frameIds.sorted())
    }

    @Test
    fun theWriterThreadFlushesRecordsWhileTheLogIsOpen() {
        val file = folder.newFile("tablet-timing.csv")
        FrameTimingLog(file, QUICK_FLUSH_MS).use { log ->
            log.record(frameInfo(1, 1, true, 1, 1, 1), FrameTimingLog.Outcome.SHOWN, 1, 1, 1, 1)
            val deadline = System.currentTimeMillis() + WAIT_TIMEOUT_MS
            while (lineCount(file) < 3 && System.currentTimeMillis() < deadline) {
                Thread.sleep(1)
            }
            assertEquals(3, lineCount(file))
        }
    }

    private fun frameInfo(
        frameId: Long,
        frameBytes: Int,
        keyframe: Boolean,
        hostCaptureUs: Long,
        firstFragmentUs: Long,
        completeUs: Long,
    ): FrameInfo =
        FrameInfo().apply {
            longArrayOf(
                frameId,
                frameBytes.toLong(),
                if (keyframe) 1 else 0,
                hostCaptureUs,
                firstFragmentUs,
                completeUs,
            ).copyInto(values)
        }

    private fun lineCount(file: File): Int = file.readLines().size

    private companion object {
        const val NEVER_FLUSH_MS = 3_600_000L
        const val QUICK_FLUSH_MS = 10L
        const val WAIT_TIMEOUT_MS = 2_000L
    }
}
