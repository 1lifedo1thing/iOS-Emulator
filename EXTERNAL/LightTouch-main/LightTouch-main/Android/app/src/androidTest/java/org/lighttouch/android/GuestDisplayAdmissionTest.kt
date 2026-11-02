package org.lighttouch.android

import android.os.Binder
import android.os.SystemClock
import android.view.MotionEvent
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.lang.reflect.Proxy

/** Production View with fake Binder responses, never guest/native acceptance. */
@RunWith(AndroidJUnit4::class)
class GuestDisplayAdmissionTest {
    private class Endpoint {
        val calls = mutableListOf<String>()
        val heldButtons = mutableSetOf<Int>()
        var rejection = ""
        var generation = 1L
        var paused = false
        val identity = Binder()
        val worker = Proxy.newProxyInstance(IEngine::class.java.classLoader, arrayOf(IEngine::class.java)) { _, method, args ->
            when (method.name) {
                "displayInputGeneration" -> if (args!![0] == 1L) generation else 0L
                "touchDisplay" -> {
                    calls += "touch"
                    when {
                        args!![1] != generation -> DisplayInputAdmission.GENERATION_RETIRED
                        paused -> DisplayInputAdmission.PAUSED
                        else -> rejection
                    }
                }
                "cancelDisplayInput" -> { calls += "cancel"; heldButtons.clear(); null }
                "button" -> {
                    val button = args!![0] as Int
                    if (args[1] as Boolean) heldButtons.add(button) else heldButtons.remove(button)
                    null
                }
                "attachDisplay" -> { calls += "surface"; "" }
                "detachDisplay" -> { check(args!![0] == 1L); calls += "detach"; null }
                "backgroundDisplay" -> { check(args!![0] == 1L); calls += "background"; null }
                "releaseDisplay" -> { check(args!![0] == 1L); calls += "release"; null }
                "performance" -> android.os.Bundle().apply { putString("cpuState", "warming up") }
                "asBinder" -> identity
                "toString" -> "Fake View admission endpoint"
                else -> error("Unexpected View control: ${method.name}")
            }
        } as IEngine
    }
    private fun fakeOwnedDisplay(endpoint: Endpoint, onError: (String) -> Unit = {}): GuestDisplay {
        val view = GuestDisplay(InstrumentationRegistry.getInstrumentation().targetContext, onError, { true }) { endpoint.worker }
        val field = GuestDisplay::class.java.getDeclaredField("lease").apply { isAccessible = true }
        (field.get(view) as WorkerDisplayLease).bind(endpoint.identity, 1)
        GuestDisplay::class.java.getDeclaredField("leasedWorker").apply { isAccessible = true }.set(view, endpoint.worker)
        return view
    }
    @Test fun destroyedSurfaceRetainsCapturedAuthorityForFollowingBackgroundPause() = onMain {
        val endpoint = Endpoint(); val display = fakeOwnedDisplay(endpoint)
        display.surfaceDestroyed(display.holder)
        assertEquals(listOf("detach"), endpoint.calls)
        display.backgroundOwnedDisplay() // The path used by the current Activity's onStop.
        assertEquals(listOf("detach", "background", "release"), endpoint.calls)
        display.retireDisplay()
        assertEquals("Repeated retirement has no captured owner left", listOf("detach", "background", "release"), endpoint.calls)
    }
    @Test fun unownedResizePreservesUnrelatedButtonAndRequiresFreshDown() = onMain {
        val endpoint = Endpoint()
        val display = fakeOwnedDisplay(endpoint)
        display.layout(0, 0, 320, 480)
        display.surfaceChanged(display.holder, 1, 320, 480)
        endpoint.worker.button(0, true)
        try {
            val before = endpoint.calls.size
            display.layout(0, 0, 320, 414)
            display.surfaceChanged(display.holder, 1, 320, 414)
            assertEquals("An unowned resize released unrelated worker input", emptyList<String>(), endpoint.calls.drop(before))
            assertEquals(setOf(0), endpoint.heldButtons)
            assertEquals(0L, display.inputObservations().getLong("cancellationRequests"))
            assertTrue(display.inputObservations().getBoolean("awaitingNewDown"))
            val resized = endpoint.calls.size
            send(display, MotionEvent.ACTION_MOVE); send(display, MotionEvent.ACTION_UP)
            assertEquals(resized, endpoint.calls.size)
            send(display, MotionEvent.ACTION_DOWN); send(display, MotionEvent.ACTION_UP)
            assertEquals(listOf("touch", "touch"), endpoint.calls.drop(resized))
            val released = endpoint.calls.size
            display.layout(0, 0, 320, 480)
            display.surfaceChanged(display.holder, 1, 320, 480)
            assertEquals("Completed contacts confer no remaining ownership", emptyList<String>(), endpoint.calls.drop(released))
            assertEquals(setOf(0), endpoint.heldButtons)
        } finally { endpoint.worker.button(0, false) }
        assertTrue(endpoint.heldButtons.isEmpty())
    }
    private fun send(display: GuestDisplay, action: Int) {
        val now = SystemClock.uptimeMillis()
        val event = MotionEvent.obtain(now, now, action, 160f, 240f, 0)
        try { assertTrue(display.dispatchTouchEvent(event)) } finally { event.recycle() }
    }
    private fun onMain(action: () -> Unit) {
        var failure: Throwable? = null
        InstrumentationRegistry.getInstrumentation().runOnMainSync {
            try { action() } catch (caught: Throwable) { failure = caught }
        }
        // Assertion failures belong to JUnit, not the application's main Looper.
        failure?.let { throw it }
    }
    @Test fun changedViewportCancelsBeforeSurfaceAndSuppressesOldContinuations() = onMain {
        val endpoint = Endpoint()
        val display = fakeOwnedDisplay(endpoint)
        display.layout(0, 0, 320, 480)
        display.surfaceChanged(display.holder, 1, 320, 480)
        assertTrue(endpoint.calls.isEmpty())
        send(display, MotionEvent.ACTION_DOWN)
        val owned = requireNotNull(display.inputObservations().getIntArray("androidContactIds"))
        assertArrayEquals(intArrayOf(0), owned)
        val before = endpoint.calls.size
        display.surfaceChanged(display.holder, 1, 320, 480)
        assertEquals(emptyList<String>(), endpoint.calls.drop(before))
        assertArrayEquals(owned, display.inputObservations().getIntArray("androidContactIds"))
        val resize = endpoint.calls.size
        display.layout(0, 0, 480, 320)
        display.surfaceChanged(display.holder, 1, 480, 320)
        assertEquals("Contact release must precede any new mapping", listOf("cancel"), endpoint.calls.drop(resize))
        assertArrayEquals(intArrayOf(), display.inputObservations().getIntArray("androidContactIds"))
        assertTrue(display.inputObservations().getBoolean("awaitingNewDown"))
        val afterResize = endpoint.calls.size
        send(display, MotionEvent.ACTION_MOVE); send(display, MotionEvent.ACTION_UP)
        assertEquals("Old contact continuation reached Binder", afterResize, endpoint.calls.size)
        send(display, MotionEvent.ACTION_DOWN); send(display, MotionEvent.ACTION_UP)
        assertEquals(listOf("touch", "touch"), endpoint.calls.drop(afterResize))
        assertArrayEquals(intArrayOf(), display.inputObservations().getIntArray("androidContactIds"))
    }
    @Test fun nonemptyNativeRejectionsDropViewOwnershipUntilFreshDown() = onMain {
        for (message in listOf("Touch left guest viewport; gesture cancelled", "Invalid touch coordinates or viewport; gesture cancelled", "No active guest frame; gesture cancelled", "No active guest session; touch was not admitted")) {
            val endpoint = Endpoint()
            val errors = mutableListOf<String>()
            val display = fakeOwnedDisplay(endpoint) { errors += it }
            display.layout(0, 0, 320, 480)
            send(display, MotionEvent.ACTION_DOWN)
            endpoint.rejection = message
            send(display, MotionEvent.ACTION_MOVE)
            assertEquals(listOf(message), errors)
            assertTrue(display.inputObservations().getBoolean("awaitingNewDown"))
            assertArrayEquals(intArrayOf(), display.inputObservations().getIntArray("androidContactIds"))
            val rejectedAt = endpoint.calls.size
            endpoint.rejection = ""
            send(display, MotionEvent.ACTION_MOVE); send(display, MotionEvent.ACTION_UP)
            assertEquals(rejectedAt, endpoint.calls.size)
            send(display, MotionEvent.ACTION_DOWN); send(display, MotionEvent.ACTION_UP)
            assertEquals(listOf("touch", "touch"), endpoint.calls.drop(rejectedAt))
        }
    }
    @Test fun balancedViewportCancellationDropsContactsAndGenerationUntilFreshDown() = onMain {
        for (hadContact in listOf(false, true)) {
            val endpoint = Endpoint(); val errors = mutableListOf<String>()
            val display = fakeOwnedDisplay(endpoint) { errors += it }
            display.layout(0, 0, 320, 480)
            if (hadContact) {
                send(display, MotionEvent.ACTION_DOWN)
                assertArrayEquals(intArrayOf(0), display.inputObservations().getIntArray("androidContactIds"))
            }
            endpoint.rejection = DisplayInputAdmission.VIEWPORT_CANCELLED
            send(display, if (hadContact) MotionEvent.ACTION_MOVE else MotionEvent.ACTION_DOWN)
            val cancelled = display.inputObservations()
            assertTrue(errors.isEmpty())
            assertEquals(1L, cancelled.getLong("expectedCancellationBatches"))
            assertEquals(0L, cancelled.getLong("rejectedBatches"))
            assertEquals(0L, cancelled.getLong("inputGeneration"))
            assertArrayEquals(intArrayOf(), cancelled.getIntArray("androidContactIds"))
            assertTrue(cancelled.getBoolean("awaitingNewDown"))
            val before = endpoint.calls.size
            endpoint.rejection = ""
            send(display, MotionEvent.ACTION_MOVE); send(display, MotionEvent.ACTION_UP)
            assertEquals("Cancelled continuation reached native admission", before, endpoint.calls.size)
            send(display, MotionEvent.ACTION_DOWN); send(display, MotionEvent.ACTION_UP)
            assertEquals(listOf("touch", "touch"), endpoint.calls.drop(before))
            assertTrue(errors.isEmpty())
        }
    }
    @Test fun viewportReleaseFailureRemainsActionableRatherThanExpectedCancellation() = onMain {
        val endpoint = Endpoint(); val errors = mutableListOf<String>()
        val display = fakeOwnedDisplay(endpoint) { errors += it }
        display.layout(0, 0, 320, 480)
        send(display, MotionEvent.ACTION_DOWN)
        val failure = "Touch left guest viewport; gesture cancelled; input release failed: native rejected"
        endpoint.rejection = failure
        send(display, MotionEvent.ACTION_MOVE)
        assertEquals(listOf(failure), errors)
        assertEquals(0L, display.inputObservations().getLong("expectedCancellationBatches"))
        assertEquals(1L, display.inputObservations().getLong("rejectedBatches"))
        assertArrayEquals(intArrayOf(), display.inputObservations().getIntArray("androidContactIds"))
        assertTrue(display.inputObservations().getBoolean("awaitingNewDown"))
    }
    @Test fun focusCancelAndGainBetweenPollsRequiresFreshDownWithoutStickyError() = onMain {
        val endpoint = Endpoint(); val errors = mutableListOf<String>()
        val display = fakeOwnedDisplay(endpoint) { errors += it }
        display.layout(0, 0, 320, 480)
        send(display, MotionEvent.ACTION_DOWN)
        assertArrayEquals(intArrayOf(0), display.inputObservations().getIntArray("androidContactIds"))
        // Model a successful native global cancel and complete focus return. The
        // View receives no focus callback/status poll between the two events.
        endpoint.paused = true; endpoint.generation++; endpoint.paused = false
        send(display, MotionEvent.ACTION_MOVE)
        assertTrue(errors.isEmpty())
        assertTrue(display.inputObservations().getBoolean("awaitingNewDown"))
        assertArrayEquals(intArrayOf(), display.inputObservations().getIntArray("androidContactIds"))
        assertEquals(1L, display.inputObservations().getLong("expectedCancellationBatches"))
        assertEquals(0L, display.inputObservations().getLong("rejectedBatches"))
        val cancelled = endpoint.calls.size
        send(display, MotionEvent.ACTION_MOVE); send(display, MotionEvent.ACTION_UP)
        assertEquals(cancelled, endpoint.calls.size)
        send(display, MotionEvent.ACTION_DOWN); send(display, MotionEvent.ACTION_UP)
        assertEquals(listOf("touch", "touch"), endpoint.calls.drop(cancelled))
        assertTrue(errors.isEmpty())
    }
    @Test fun pausedFreshDownIsNotAdmittedAndCannotContinueAfterResume() = onMain {
        val endpoint = Endpoint(); val errors = mutableListOf<String>()
        val display = fakeOwnedDisplay(endpoint) { errors += it }
        display.layout(0, 0, 320, 480)
        endpoint.paused = true
        send(display, MotionEvent.ACTION_DOWN)
        assertArrayEquals(intArrayOf(), display.inputObservations().getIntArray("androidContactIds"))
        endpoint.paused = false
        val blocked = endpoint.calls.size
        send(display, MotionEvent.ACTION_MOVE); send(display, MotionEvent.ACTION_UP)
        assertEquals(blocked, endpoint.calls.size)
        send(display, MotionEvent.ACTION_DOWN); send(display, MotionEvent.ACTION_UP)
        assertEquals(listOf("touch", "touch"), endpoint.calls.drop(blocked))
        assertTrue(errors.isEmpty())
    }
}
