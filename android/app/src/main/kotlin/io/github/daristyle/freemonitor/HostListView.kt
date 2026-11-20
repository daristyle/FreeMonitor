package io.github.daristyle.freemonitor

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class HostListView(
    context: Context,
    private val onHostChosen: (DiscoveredHost) -> Unit,
    private val onSearchAgain: () -> Unit,
    private val onClose: () -> Unit,
) : FrameLayout(context) {
    private val noticeView = label(NOTICE_TEXT_SIZE_SP)
    private val titleView = label(TITLE_TEXT_SIZE_SP)
    private val hostRows = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private val closeButton = button(context.getString(R.string.host_list_close)) { onClose() }

    val isOpen: Boolean
        get() = visibility == VISIBLE

    val closesOnBack: Boolean
        get() = isOpen && closeButton.visibility == VISIBLE

    init {
        setBackgroundColor(BACKGROUND_COLOR)
        isClickable = true
        visibility = GONE
        val buttons =
            LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER
                addView(button(context.getString(R.string.host_search_again)) { onSearchAgain() })
                addView(closeButton)
            }
        val padding = pixels(PANEL_PADDING_DP)
        val panel =
            LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                minimumWidth = pixels(PANEL_MIN_WIDTH_DP)
                setPadding(padding, padding, padding, padding)
                addView(noticeView, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
                addView(titleView, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
                addView(hostRows, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
                addView(buttons, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            }
        addView(ScrollView(context).apply { addView(panel) }, LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.CENTER))
    }

    fun open(
        notice: String?,
        closable: Boolean,
    ) {
        noticeView.text = notice.orEmpty()
        noticeView.visibility = if (notice == null) GONE else VISIBLE
        closeButton.visibility = if (closable) VISIBLE else GONE
        visibility = VISIBLE
    }

    fun close() {
        visibility = GONE
    }

    fun showSearching() {
        titleView.setText(R.string.host_list_searching)
        hostRows.removeAllViews()
    }

    fun showHosts(
        hosts: List<DiscoveredHost>,
        appVersion: Int,
    ) {
        titleView.setText(if (hosts.isEmpty()) R.string.host_list_none else R.string.host_list_title)
        hostRows.removeAllViews()
        for (host in hosts) {
            hostRows.addView(hostRow(host, host.compatibility(appVersion)))
        }
    }

    fun showFailure(message: String) {
        titleView.text = context.getString(R.string.host_list_failed, message)
        hostRows.removeAllViews()
    }

    private fun hostRow(
        host: DiscoveredHost,
        compatibility: HostCompatibility,
    ): Button {
        val text =
            when (compatibility) {
                HostCompatibility.COMPATIBLE -> R.string.host_row
                HostCompatibility.HOST_IS_OLDER -> R.string.host_row_host_older
                HostCompatibility.APP_IS_OLDER -> R.string.host_row_app_older
            }
        val name = if (host.tethering) context.getString(R.string.host_name_tethering, host.name) else host.name
        return button(context.getString(text, name, host.endpoint)) { onHostChosen(host) }.apply {
            isEnabled = compatibility == HostCompatibility.COMPATIBLE
            textSize = ROW_TEXT_SIZE_SP
        }
    }

    private fun label(textSizeSp: Float): TextView =
        TextView(context).apply {
            gravity = Gravity.CENTER
            textSize = textSizeSp
            setTextColor(Color.WHITE)
            val padding = pixels(LABEL_PADDING_DP)
            setPadding(0, padding, 0, padding)
        }

    private fun button(
        text: String,
        onClick: () -> Unit,
    ): Button =
        Button(context).apply {
            this.text = text
            isAllCaps = false
            setOnClickListener { onClick() }
        }

    private fun pixels(dp: Float): Int = (dp * resources.displayMetrics.density).toInt()

    private companion object {
        const val BACKGROUND_COLOR = 0xE0000000.toInt()
        const val NOTICE_TEXT_SIZE_SP = 18f
        const val TITLE_TEXT_SIZE_SP = 24f
        const val ROW_TEXT_SIZE_SP = 18f
        const val PANEL_PADDING_DP = 24f
        const val PANEL_MIN_WIDTH_DP = 400f
        const val LABEL_PADDING_DP = 8f
    }
}
