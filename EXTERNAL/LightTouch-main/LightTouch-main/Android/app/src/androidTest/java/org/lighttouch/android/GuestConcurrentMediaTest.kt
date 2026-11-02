package org.lighttouch.android

import android.content.Intent
import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Actual concurrent production input/output, never substitutes cumulative boot counters. */
@RunWith(AndroidJUnit4::class)
class GuestConcurrentMediaTest {
    @Test fun twoContactsOverlapGuestScreenshotSoundAndFramePresentation() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Existing real guest and intentional guest screenshot required", arguments.getString("exerciseConcurrentMedia") == "true")
        val context = instrumentation.targetContext
        val evidence = File(context.filesDir, "concurrent-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("hostChecksPassed", false).put("guestAcceptancePassed", false)
            .put("visibleGuestVerified", false).put("audiblePlaybackVerified", false)
            .put("guestContactConsumptionVerified", false).put("guestPhotoCreatedVerified", false)
            .put("workload", "Two production touch contacts plus fixed guest Home/Power screenshot chord; may create a photo in writable guest state")
        val samples = JSONArray(); val events = JSONArray(); val intervals = JSONArray()
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is IntArray -> put(key, JSONArray(value.toList()))
                is LongArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value)
            }
        }
        fun persist() {
            File(evidence, "result.json").writeText(result.toString(2))
            File(evidence, "samples.json").writeText(samples.toString(2))
            File(evidence, "events.json").writeText(events.toString(2))
            File(evidence, "intervals.json").writeText(intervals.toString(2))
        }
        fun findDisplay(view: View): GuestDisplay? {
            if (view is GuestDisplay) return view
            if (view is ViewGroup) for (i in 0 until view.childCount) findDisplay(view.getChildAt(i))?.let { return it }
            return null
        }
        try {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                    val worker = binding.get()
                    val initial = worker.status()
                    val pid = initial.getInt("pid")
                    val wire = if (arguments.getString("touchWireTrace") == "true") GuestTouchWireEvidence(context.filesDir, initial, evidence) else null
                    var touchIndex = 0
                    assertEquals(Process.myUid(), initial.getInt("uid")); assertNotEquals(Process.myPid(), pid)
                    assertTrue("Boot and visually inspect the existing guest first; this test never starts one", initial.getString("state") in listOf("running", "paused"))
                    if (initial.getString("state") == "paused") worker.pause(false)
                    val runningDeadline = SystemClock.elapsedRealtime() + 15_000
                    while (worker.status().getString("state") != "running" && SystemClock.elapsedRealtime() < runningDeadline) Thread.sleep(100)
                    val seconds = arguments.getString("concurrentSeconds", "10").toInt().coerceIn(3, 30)
                    val settle = arguments.getString("postGestureSettleSeconds", "5").toInt().coerceIn(0, 30)
                    result.put("workerPid", pid).put("appUid", Process.myUid()).put("heldMeasurementSeconds", seconds)
                        .put("postGestureSettleSeconds", settle).put("counterClock", "Status read bounds use host elapsed realtime; source input uses uptime milliseconds. No cross-clock subtraction.")
                    fun observe(label: String, contactsHeld: Boolean): Bundle {
                        val begin = SystemClock.elapsedRealtimeNanos()
                        val status = worker.status()
                        val end = SystemClock.elapsedRealtimeNanos()
                        val item = JSONObject().put("label", label).put("readBeginElapsedNs", begin).put("readEndElapsedNs", end).put("status", encode(status))
                        scenario.onActivity { activity ->
                            val display = requireNotNull(findDisplay(activity.window.decorView)) { "Production GuestDisplay missing" }
                            val input = display.inputObservations()
                            item.put("androidInput", encode(input)).put("surfaceValid", display.holder.surface.isValid)
                                .put("displayRotation", display.display.rotation)
                            if (contactsHeld) assertArrayEquals("Both Android-owned contacts must remain held", intArrayOf(7, 23), input.getIntArray("androidContactIds"))
                        }
                        samples.put(item); persist()
                        wire?.drain(touchIndex - 1)
                        assertEquals("Worker changed", pid, status.getInt("pid"))
                        assertEquals("Production worker error", "", status.getString("error"))
                        assertEquals("Foreground running VM required", "running", status.getString("state"))
                        return requireNotNull(status.getBundle("observations"))
                    }
                    fun capture(label: String) {
                        val target = File(context.filesDir, "screenshots/${evidence.name}/$label.png")
                        val begin = SystemClock.elapsedRealtimeNanos()
                        val error = worker.screenshot(target.absolutePath)
                        events.put(JSONObject().put("capture", label).put("path", target.absolutePath).put("readBeginElapsedNs", begin)
                            .put("readEndElapsedNs", SystemClock.elapsedRealtimeNanos()).put("error", error)); persist()
                        assertEquals("Actual guest capture", "", error)
                    }
                    val downTime = SystemClock.uptimeMillis()
                    fun touch(action: Int, fractions: List<Pair<Float, Float>>) {
                        val status = worker.status() // Native observation stays off the Activity thread.
                        scenario.onActivity { activity ->
                            val display = requireNotNull(findDisplay(activity.window.decorView))
                            val viewport = GuestTestViewport(status, display.width, display.height)
                            val ids = if (fractions.size == 1) intArrayOf(7) else intArrayOf(7, 23)
                            val properties = ids.map { MotionEvent.PointerProperties().apply { id = it; toolType = MotionEvent.TOOL_TYPE_FINGER } }.toTypedArray()
                            val points = fractions.map { viewport.point(it.first, it.second) }
                            val coordinates = points.map { MotionEvent.PointerCoords().apply { x = it.first; y = it.second; pressure = 1f; size = .05f } }.toTypedArray()
                            val at = SystemClock.uptimeMillis()
                            val event = MotionEvent.obtain(downTime, at, action, ids.size, properties, coordinates, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
                            try { assertTrue(display.dispatchTouchEvent(event)) } finally { event.recycle() }
                            events.put(JSONObject().put("touchIndex", touchIndex++).put("action", action).put("rawUptimeMs", at).put("ids", JSONArray(ids.toList()))
                                .put("viewport", viewport.receipt()).put("viewPixels", JSONArray(points.map { JSONArray(listOf(it.first, it.second)) })))
                        }
                        persist()
                    }
                    var homeHeld = false; var powerHeld = false
                    fun button(button: Int, down: Boolean) {
                        // Track intended ownership before admission so finally also releases after exceptions.
                        if (down) { if (button == 0) homeHeld = true else powerHeld = true }
                        events.put(JSONObject().put("button", if (button == 0) "Home" else "Power").put("down", down)
                            .put("requestElapsedNs", SystemClock.elapsedRealtimeNanos())); persist()
                        worker.button(button, down)
                        if (!down) { if (button == 0) homeHeld = false else powerHeld = false }
                    }
                    var failure: Throwable? = null
                    try {
                        val before = observe("ready-before-touch", false)
                        assertTrue("Guest display is sleeping", before.getLong("displaySleeping") == 0L)
                        assertTrue("No actual production presentation yet", before.getLong("presentedFrames") > 0)
                        capture("before")
                        touch(MotionEvent.ACTION_DOWN, listOf(.42f to .5f))
                        touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), listOf(.42f to .5f, .58f to .5f))
                        var previous = observe("two-held-before-chord", true)
                        val heldBaseline = previous
                        val start = SystemClock.elapsedRealtime(); val deadline = start + seconds * 1000L
                        var chord = false; var overlappingIntervals = 0
                        while (SystemClock.elapsedRealtime() < deadline) {
                            val t = ((SystemClock.elapsedRealtime() - start).toFloat() / (seconds * 1000f)).coerceIn(0f, 1f)
                            touch(MotionEvent.ACTION_MOVE, listOf((.42f - .12f * t) to .5f, (.58f + .12f * t) to .5f))
                            if (!chord && SystemClock.elapsedRealtime() - start >= 500) {
                                button(0, true); button(1, true); Thread.sleep(150)
                                button(1, false); button(0, false); chord = true
                            }
                            Thread.sleep(100)
                            val measured = observe("held-moving", true)
                            val oldAudio = requireNotNull(previous.getBundle("audio")); val audio = requireNotNull(measured.getBundle("audio"))
                            val interval = JSONObject().put("fromSample", samples.length() - 2).put("toSample", samples.length() - 1)
                                .put("twoAndroidContactsHeldThroughout", true).put("guestConsumptionInferred", false)
                            fun delta(key: String, old: Bundle, new: Bundle): Long = (new.getLong(key) - old.getLong(key)).also { interval.put(key + "Delta", it) }
                            val produced = delta("producedFrames", previous, measured)
                            val presented = delta("presentedFrames", previous, measured)
                            val pulled = delta("pulledNonzeroSamples", oldAudio, audio)
                            val written = delta("writtenNonzeroSamples", oldAudio, audio)
                            val played = delta("playedFrames", oldAudio, audio)
                            delta("underruns", oldAudio, audio)
                            val overlap = produced > 0 && presented > 0 && pulled > 0 && written > 0 && played > 0
                            interval.put("frameAndNonzeroPlaybackDeltasInSameHeldInterval", overlap)
                            intervals.put(interval); persist()
                            if (overlap) overlappingIntervals++
                            assertEquals("Audio write failures", oldAudio.getLong("writeFailures"), audio.getLong("writeFailures"))
                            assertEquals("Native PCM failures", oldAudio.getLong("nativePullFailures"), audio.getLong("nativePullFailures"))
                            assertEquals("Window presentation failures", previous.getLong("windowFailures"), measured.getLong("windowFailures"))
                            previous = measured
                        }
                        capture("two-held-after-chord")
                        result.put("overlappingHeldIntervals", overlappingIntervals).put("screenshotChordAttempted", chord)
                            .put("heldFrameDelta", previous.getLong("presentedFrames") - heldBaseline.getLong("presentedFrames"))
                        persist()
                        touch(MotionEvent.ACTION_POINTER_UP or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), listOf(.30f to .5f, .70f to .5f))
                        touch(MotionEvent.ACTION_UP, listOf(.30f to .5f))
                        Thread.sleep(settle * 1000L); capture("after-settled")
                        val terminal = observe("settled-before-scenario-close", false)
                        val downs = requireNotNull(terminal.getLongArray("buttonDownAdmissions")); val ups = requireNotNull(terminal.getLongArray("buttonUpAdmissions"))
                        for (id in 0..1) {
                            assertEquals("Exactly one chord press", 1L, downs[id] - requireNotNull(before.getLongArray("buttonDownAdmissions"))[id])
                            assertEquals("Balanced chord release", 1L, ups[id] - requireNotNull(before.getLongArray("buttonUpAdmissions"))[id])
                        }
                        assertEquals(before.getLong("buttonRejections"), terminal.getLong("buttonRejections"))
                        assertTrue("No measured interval combined two held Android contacts, fresh frame presentation and nonzero guest playback", overlappingIntervals > 0)
                        if (wire != null) {
                            wire.drain(touchIndex - 1)
                            result.put("retainedPendingFrameReads", wire.collectedFrameCount()).put("retainedSerializedContactRecords", wire.serializedContactCount())
                            assertTrue("No retained actual pending guest FRAME_READ", wire.collectedFrameCount() > 0)
                            assertTrue("No retained serialized nonempty guest contacts", wire.serializedContactCount() > 0)
                        }
                        result.put("hostChecksPassed", true)
                    } catch (e: Throwable) { failure = e; throw e }
                    finally {
                        var cleanup: Throwable? = null
                        fun release(action: () -> Unit) { try { action() } catch (e: Throwable) { if (cleanup == null) cleanup = e else cleanup!!.addSuppressed(e) } }
                        if (powerHeld) release { button(1, false) }
                        if (homeHeld) release { button(0, false) }
                        release { scenario.onActivity { findDisplay(it.window.decorView)?.cancelOwnedGesture() } }
                        release { worker.cancelInput() }
                        release { wire?.drain(touchIndex - 1) }
                        cleanup?.let { e -> result.put("cleanupFailure", e.toString()); if (failure != null) failure!!.addSuppressed(e) else throw e }
                        persist()
                    }
                }
            }
        } catch (failure: Throwable) { result.put("failure", failure.toString()); throw failure }
        finally { persist() }
    }
}
