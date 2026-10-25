package io.github.daristyle.freemonitor

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
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

class MainActivity :
    Activity(),
    SurfaceHolder.Callback {
    private lateinit var waitingView: TextView
    private lateinit var statusView: TextView
    private var receiver: FrameReceiver? = null
    private var renderer: FrameRenderer? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val surfaceView = SurfaceView(this).also { it.holder.addCallback(this) }
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
        setContentView(
            FrameLayout(this).apply {
                setBackgroundColor(Color.BLACK)
                addView(surfaceView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
                addView(waitingView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
                addView(
                    statusView,
                    FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.BOTTOM or Gravity.START),
                )
            },
        )
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
        renderer = FrameRenderer(holder, newReceiver, ::onRenderStatus).also { it.start() }
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
        receiver?.close()
        receiver = null
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
                        status.statistics.framesDropped,
                        status.statistics.framesReplaced,
                        status.statistics.invalidPackets,
                        status.notJPEGFrames,
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
    }
}
