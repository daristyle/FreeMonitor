package io.github.daristyle.freemonitor

import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HostListViewTests {
    @Test
    fun backClosesAnOpenListThatHasACloseButton() =
        onMainThread { view ->
            assertFalse(view.closesOnBack)
            view.open(notice = null, closable = true)
            assertTrue(view.closesOnBack)
            view.close()
            assertFalse(view.closesOnBack)
        }

    @Test
    fun backLeavesAListWithoutACloseButtonAlone() =
        onMainThread { view ->
            view.open(notice = null, closable = false)
            assertFalse(view.closesOnBack)
        }

    private fun onMainThread(check: (HostListView) -> Unit) {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        instrumentation.runOnMainSync {
            check(HostListView(instrumentation.targetContext, onHostChosen = {}, onSearchAgain = {}, onClose = {}))
        }
    }
}
