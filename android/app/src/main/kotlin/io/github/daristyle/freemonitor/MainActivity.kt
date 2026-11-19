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
import kotlin.math.roundToInt

class MainActivity :
    Activity(),
    SurfaceHolder.Callback {
    private lateinit var container: FrameLayout
    private lateinit var waitingView: TextView
    private lateinit var statusView: TextView
    private lateinit var cursorView: CursorView
    private var surfaceView: SurfaceView? = null
    private var receiver: FrameReceiver? = null
    private var timingLog: FrameTimingLog? = null
    private var renderer: FrameRenderer? = null
    private var sessionHost: String? = null

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
        cursorView = CursorView(this)
        container =
            FrameLayout(this).apply {
                setBackgroundColor(Color.BLACK)
                addView(cursorView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
                addView(waitingView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
                addView(
                    statusView,
                    FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.BOTTOM or Gravity.START),
                )
            }
        setContentView(container)
        container.setOnClickListener { receiver?.reconnect() }
        sessionHost = chooseSessionHost()
        addSurfaceView()
        window.insetsController?.let { controller ->
            controller.hide(WindowInsets.Type.systemBars())
            controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        showWaiting()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val host = sessionHost
        val newReceiver =
            try {
                if (host == null) FrameReceiver.development(RECEIVE_PORT) else openSessionReceiver(host)
            } catch (error: IllegalStateException) {
                waitingView.text =
                    if (host == null) {
                        getString(R.string.receiver_failed, RECEIVE_PORT, error.message)
                    } else {
                        getString(R.string.session_failed, host, error.message)
                    }
                return
            }
        if (host != null) {
            saveHost(host)
        }
        receiver = newReceiver
        cursorView.start(newReceiver)
        timingLog = if (intent.getBooleanExtra(TIMING_LOG_EXTRA, false)) openTimingLog() else null
        val view = surfaceView
        renderer =
            FrameRenderer(
                holder,
                newReceiver,
                timingLog,
                ::onRenderStatus,
                onVideoSize = { size -> container.post { onVideoSize(view, size) } },
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
        cursorView.stop()
        renderer?.stop()
        renderer = null
        timingLog?.let(::closeTimingLog)
        timingLog = null
        receiver?.close()
        receiver = null
    }

    private fun chooseSessionHost(): String? {
        if (intent.getBooleanExtra(DEVELOPMENT_EXTRA, false)) {
            return null
        }
        val requested = intent.getStringExtra(HOST_EXTRA)?.trim()?.takeIf { it.isNotEmpty() }
        val host = requested ?: getPreferences(MODE_PRIVATE).getString(HOST_PREFERENCE, null)
        return host?.let { if (':' in it) it else "$it:$HOST_PORT" }
    }

    private fun saveHost(host: String) {
        getPreferences(MODE_PRIVATE).edit().putString(HOST_PREFERENCE, host).apply()
    }

    private fun openSessionReceiver(host: String): FrameReceiver {
        val mode = display?.mode
        val metrics = resources.displayMetrics
        val width =
            mode?.let { maxOf(it.physicalWidth, it.physicalHeight) } ?: maxOf(metrics.widthPixels, metrics.heightPixels)
        val height =
            mode?.let { minOf(it.physicalWidth, it.physicalHeight) } ?: minOf(metrics.widthPixels, metrics.heightPixels)
        val refreshHz = (mode?.refreshRate ?: DEFAULT_REFRESH_HZ).roundToInt()
        return FrameReceiver.session(host, width, height, refreshHz)
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

    private fun onVideoSize(
        view: SurfaceView?,
        size: VideoSize,
    ) {
        if (view == null || view !== surfaceView) {
            return
        }
        cursorView.setVideoSize(size)
        fitSurfaceView(view, size)
    }

    private fun fitSurfaceView(
        view: SurfaceView,
        size: VideoSize,
    ) {
        if (container.width == 0 || container.height == 0) {
            return
        }
        val fit = FitRectangle.fit(size.width, size.height, container.width, container.height)
        view.layoutParams = FrameLayout.LayoutParams(fit.width, fit.height, Gravity.CENTER)
    }

    private fun showWaiting() {
        val host = sessionHost
        if (host != null) {
            waitingView.text = getString(R.string.session_connecting, host)
            return
        }
        val address = NetworkAddress.localIPAddress(this) ?: getString(R.string.unknown_address)
        waitingView.text = getString(R.string.waiting_for_host, address, RECEIVE_PORT)
    }

    private fun onRenderStatus(status: RenderStatus) {
        statusView.post {
            val session = status.session
            val sinceLastFrame = status.millisecondsSinceLastFrame
            val showingVideo =
                sinceLastFrame != null &&
                    sinceLastFrame < NO_FRAMES_TIMEOUT_MS &&
                    (session == null || session.state == SessionState.CONNECTED)
            if (!showingVideo) {
                if (session == null) showWaiting() else waitingView.text = sessionText(session)
                waitingView.visibility = TextView.VISIBLE
                statusView.text = ""
            } else {
                waitingView.visibility = TextView.GONE
                statusView.text = renderStatusText(status)
            }
        }
    }

    private fun renderStatusText(status: RenderStatus): String {
        val text =
            getString(
                R.string.render_status,
                status.framesPerSecond,
                status.codec?.label ?: getString(R.string.no_codec),
                status.statistics.framesDropped,
                status.statistics.framesRepaired,
                status.statistics.framesReplaced,
                status.statistics.invalidPackets,
                status.framesWaitingForKeyframe,
            )
        val roundTripUs = status.session?.roundTripUs ?: return text
        return text + getString(R.string.round_trip_status, roundTripUs / MICROSECONDS_PER_MILLISECOND)
    }

    private fun sessionText(session: SessionStatus): String {
        val host = sessionHost.orEmpty()
        return when (session.state) {
            SessionState.CONNECTED -> getString(R.string.session_connected, host)
            SessionState.REJECTED -> getString(R.string.session_rejected, host, rejectReasonText(session.rejectReason))
            SessionState.ENDED_BY_HOST -> getString(R.string.session_ended, host)
            SessionState.CONNECTING, SessionState.IDLE -> getString(R.string.session_connecting, host)
        }
    }

    private fun rejectReasonText(reason: RejectReason): String =
        getString(
            when (reason) {
                RejectReason.UNSUPPORTED_VERSION -> R.string.reject_unsupported_version
                RejectReason.NO_COMMON_CODEC -> R.string.reject_no_common_codec
                RejectReason.INVALID_PARAMETERS -> R.string.reject_invalid_parameters
                RejectReason.MISSING_FEATURE -> R.string.reject_missing_feature
                RejectReason.UNKNOWN -> R.string.reject_unknown
            },
        )

    private companion object {
        const val RECEIVE_PORT = 50500
        const val HOST_PORT = 50500
        const val DEFAULT_REFRESH_HZ = 60f
        const val MICROSECONDS_PER_MILLISECOND = 1000f
        const val NO_FRAMES_TIMEOUT_MS = 3000L
        const val WAITING_TEXT_SIZE_SP = 28f
        const val STATUS_TEXT_SIZE_SP = 12f
        const val STATUS_ALPHA = 0.6f
        const val STATUS_PADDING_DP = 8f
        const val TAG = "FreeMonitor"
        const val TIMING_LOG_EXTRA = "timingLog"
        const val HOST_EXTRA = "host"
        const val DEVELOPMENT_EXTRA = "development"
        const val HOST_PREFERENCE = "host"
        const val TIMING_DIRECTORY = "timing"
        val timingFileTimeFormat: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")
    }
}
