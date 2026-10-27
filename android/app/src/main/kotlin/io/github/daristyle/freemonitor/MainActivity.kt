package io.github.daristyle.freemonitor

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import java.io.File
import java.io.IOException
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

class MainActivity :
    Activity(),
    SurfaceHolder.Callback {
    private lateinit var container: FrameLayout
    private lateinit var waitingView: TextView
    private lateinit var statusView: TextView
    private var surfaceView: SurfaceView? = null
    private var receiver: FrameReceiver? = null
    private var timingLog: FrameTimingLog? = null
    private var renderer: FrameRenderer? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        waitingView =
            TextView(this).apply {
                gravity = Gravity.CENTER
                textSize = WAITING_TEXT_SIZE_SP
                setTextColor(Color.WHITE)
            }
        statusView =
            TextView(this).apply {
                textSize = STATUS_TEXT_SIZE_SP
                setTextColor(Color.WHITE)
                alpha = STATUS_ALPHA
                val padding = (STATUS_PADDING_DP * resources.displayMetrics.density).toInt()
                setPadding(padding, padding, padding, padding)
            }
        container =
            FrameLayout(this).apply {
                setBackgroundColor(Color.BLACK)
                addView(waitingView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
                addView(
                    statusView,
                    FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.BOTTOM or Gravity.START),
                )
            }
        setContentView(container)
        addSurfaceView()
        window.insetsController?.let { controller ->
            controller.hide(WindowInsets.Type.systemBars())
            controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        showWaiting()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val newReceiver =
            try {
                FrameReceiver(RECEIVE_PORT)
            } catch (error: IllegalStateException) {
                waitingView.text = getString(R.string.receiver_failed, RECEIVE_PORT, error.message)
                return
            }
        receiver = newReceiver
        timingLog = if (intent.getBooleanExtra(TIMING_LOG_EXTRA, false)) openTimingLog() else null
        val view = surfaceView
        renderer =
            FrameRenderer(
                holder,
                newReceiver,
                timingLog,
                ::onRenderStatus,
                onVideoSize = { size -> container.post { fitSurfaceView(view, size) } },
                onCodecChanged = { container.post { replaceSurfaceView(view) } },
            ).also { it.start() }
    }

    override fun surfaceChanged(
        holder: SurfaceHolder,
        format: Int,
        width: Int,
        height: Int,
    ) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        renderer?.stop()
        renderer = null
        timingLog?.let(::closeTimingLog)
        timingLog = null
        receiver?.close()
        receiver = null
    }

    private fun openTimingLog(): FrameTimingLog? {
        val directory = getExternalFilesDir(TIMING_DIRECTORY) ?: return null
        val file = File(directory, "tablet-timing-${LocalDateTime.now().format(timingFileTimeFormat)}.csv")
        return try {
            FrameTimingLog(file).also { Log.i(TAG, "Writing frame timings to ${file.absolutePath}") }
        } catch (error: IOException) {
            Log.w(TAG, "Could not create the timing log ${file.absolutePath}", error)
            null
        }
    }

    private fun closeTimingLog(log: FrameTimingLog) {
        log.close()
        if (log.failed) {
            Log.w(TAG, "Writing the timing log failed")
        }
        if (log.droppedRecordCount > 0) {
            Log.w(TAG, "${log.droppedRecordCount} frame timings were dropped because the log could not keep up")
        }
    }

    private fun addSurfaceView() {
        val view = SurfaceView(this).also { it.holder.addCallback(this) }
        container.addView(view, 0, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        surfaceView = view
    }

    private fun replaceSurfaceView(view: SurfaceView?) {
        if (view == null || view !== surfaceView) {
            return
        }
        container.removeView(view)
        addSurfaceView()
    }

    private fun fitSurfaceView(
        view: SurfaceView?,
        size: VideoSize,
    ) {
        if (view == null || view !== surfaceView || container.width == 0 || container.height == 0) {
            return
        }
        val fit = FitRectangle.fit(size.width, size.height, container.width, container.height)
        view.layoutParams = FrameLayout.LayoutParams(fit.width, fit.height, Gravity.CENTER)
    }

    private fun showWaiting() {
        val address = NetworkAddress.localIPAddress(this) ?: getString(R.string.unknown_address)
        waitingView.text = getString(R.string.waiting_for_host, address, RECEIVE_PORT)
    }

    private fun onRenderStatus(status: RenderStatus) {
        statusView.post {
            val sinceLastFrame = status.millisecondsSinceLastFrame
            if (sinceLastFrame == null || sinceLastFrame >= NO_FRAMES_TIMEOUT_MS) {
                showWaiting()
                waitingView.visibility = TextView.VISIBLE
                statusView.text = ""
            } else {
                waitingView.visibility = TextView.GONE
                statusView.text =
                    getString(
                        R.string.render_status,
                        status.framesPerSecond,
                        status.codec?.label ?: getString(R.string.no_codec),
                        status.statistics.framesDropped,
                        status.statistics.framesReplaced,
                        status.statistics.invalidPackets,
                        status.framesWaitingForKeyframe,
                    )
            }
        }
    }

    private companion object {
        const val RECEIVE_PORT = 50500
        const val NO_FRAMES_TIMEOUT_MS = 3000L
        const val WAITING_TEXT_SIZE_SP = 28f
        const val STATUS_TEXT_SIZE_SP = 12f
        const val STATUS_ALPHA = 0.6f
        const val STATUS_PADDING_DP = 8f
        const val TAG = "FreeMonitor"
        const val TIMING_LOG_EXTRA = "timingLog"
        const val TIMING_DIRECTORY = "timing"
        val timingFileTimeFormat: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")
    }
}
