package org.lighttouch.android

import android.content.*
import android.os.*
import android.view.*
import androidx.lifecycle.Lifecycle
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
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Root-observed n72 Lock layout only; retained diagnosis is not an unlock verdict. */
@RunWith(AndroidJUnit4::class)
class GuestUnlockTest {
    private fun findDisplay(view: View): GuestDisplay? {
        if (view is GuestDisplay) return view
        if (view is ViewGroup) for (index in 0 until view.childCount) findDisplay(view.getChildAt(index))?.let { return it }
        return null
    }
    /** No AUTO_CREATE: this harness must bind a service that already exists. */
    private class ExistingWorker(private val context: Context) : AutoCloseable {
        private val connected = CountDownLatch(1)
        private var worker: IEngine? = null
        private val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                worker = IEngine.Stub.asInterface(binder); connected.countDown()
            }
            override fun onServiceDisconnected(name: ComponentName) = Unit
        }
        init { check(context.bindService(Intent(context, EngineService::class.java), connection, 0)) { "Existing engine service required" } }
        fun get(): IEngine {
            check(connected.await(10, TimeUnit.SECONDS)) { "Existing worker connection timed out" }
            return requireNotNull(worker)
        }
        override fun close() { context.unbindService(connection) }
    }
    @Test fun wakeObservedLockAndSwipeThroughProductionDisplay() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit unlock diagnosis required", arguments.getString("exerciseGuestUnlock") == "true")
        assumeTrue("Root must have inspected the n72 Lock layout", arguments.getString("observedN72LockLayout") == "true")
        val expectedApk = requireNotNull(arguments.getString("expectedMainApkSha256")).also {
            require(it.matches(Regex("[0-9a-f]{64}"))) { "Exact main APK SHA256 required" }
        }
        val pid = requireNotNull(arguments.getString("expectedWorkerPid")?.toIntOrNull()).also { require(it > 0) }
        val ticks = requireNotNull(arguments.getString("expectedWorkerStartTicks")).also { require((it.toLongOrNull() ?: 0) > 0) }
        val boot = requireNotNull(arguments.getString("expectedBootId")).also {
            require(it.matches(Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")))
        }
        val context = instrumentation.targetContext
        val evidence = File(context.filesDir, "unlock-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("hostExerciseCompleted", false).put("guestAcceptancePassed", false)
            .put("visibleGuestVerified", false).put("guestUnlockedVerified", false).put("audiblePlaybackVerified", false)
            .put("observedN72LockLayoutOptIn", true).put("automaticScreenClassification", false)
            .put("hostDeadlineClock", "elapsedRealtime (BOOTTIME)").put("motionEventClock", "uptimeMillis (MONOTONIC)")
            .put("guestVirtualDwellMeasured", false)
            .put("expectedMainApkSha256", expectedApk).put("expectedWorkerPid", pid)
            .put("expectedWorkerStartTicks", ticks).put("expectedBootId", boot)
        val timeline = JSONArray()
        val events = JSONArray()
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is IntArray -> put(key, JSONArray(value.toList()))
                is LongArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value ?: JSONObject.NULL)
            }
        }
        fun persist() {
            File(evidence, "result.json").writeText(result.toString(2))
            File(evidence, "timeline.json").writeText(timeline.toString(2))
            File(evidence, "motion-events.json").writeText(events.toString(2))
        }
        fun processIdentity() {
            assertNotEquals(Process.myPid(), pid)
            assertEquals("Android boot changed", boot, File("/proc/sys/kernel/random/boot_id").readText().trim())
            val fields = File("/proc/$pid/stat").readText().substringAfterLast(')').trim().split(Regex("\\s+"))
            assertEquals("Worker PID reused", ticks, fields[19])
            assertFalse("Worker retired", fields[0] in listOf("Z", "X"))
            val uid = File("/proc/$pid/status").useLines { rows -> rows.first { it.startsWith("Uid:") } }
                .substringAfter(':').trim().split(Regex("\\s+"))[0].toInt()
            assertEquals("Worker UID changed", Process.myUid(), uid)
        }
        try {
            val installed = acceptanceSha256(File(context.applicationInfo.sourceDir))
            result.put("installedMainApkSha256", installed)
            assertEquals("Installed main APK identity", expectedApk, installed)
            processIdentity()
            ExistingWorker(context).use { binding ->
                val worker = binding.get()
                processIdentity()
                val initial = worker.status()
                assertTrue("Existing guest required; no Start/recovery is performed", initial.getBoolean("sessionStarted"))
                assertTrue("Existing running/paused guest required", initial.getString("state") in listOf("running", "paused"))
                assertEquals("Existing worker binding", pid, initial.getInt("pid"))
                assertEquals(Process.myUid(), initial.getInt("uid")); assertTrue(pid > 0); assertNotEquals(Process.myPid(), pid)
                val baselineFrames = requireNotNull(initial.getBundle("observations")).getLong("producedFrames")
                result.put("initialWorker", encode(initial)).put("workerPid", pid).put("appUid", Process.myUid())
                    .put("initialProducerFrames", baselineFrames)
                persist()
                ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                    fun status(label: String): Bundle {
                        processIdentity()
                        val state = worker.status()
                        timeline.put(JSONObject().put("label", label).put("hostElapsedMs", SystemClock.elapsedRealtime()).put("status", encode(state)))
                        assertEquals("Worker replaced", pid, state.getInt("pid")); assertEquals(Process.myUid(), state.getInt("uid"))
                        assertTrue("Session disappeared", state.getBoolean("sessionStarted"))
                        check(state.getString("state") in listOf("running", "paused")) { state.toString() }
                        return state
                    }
                    fun onActivity(action: (MainActivity) -> Unit) {
                        var failure: Throwable? = null
                        scenario.onActivity { activity -> try { action(activity) } catch (caught: Throwable) { failure = caught } }
                        failure?.let { throw it }
                    }
                    fun capture(label: String, snapshot: Bundle? = null): String {
                        assertEquals("Activity must remain resumed through captures", Lifecycle.State.RESUMED, scenario.state)
                        val state = snapshot ?: status(label)
                        val target = File(context.filesDir, "screenshots/${evidence.name}/$label.png")
                        processIdentity()
                        val error = worker.screenshot(target.absolutePath)
                        timeline.put(JSONObject().put("capture", target.absolutePath).put("label", label)
                            .put("hostElapsedMs", SystemClock.elapsedRealtime()).put("captureError", error)
                            .put("producerFrames", state.getBundle("observations")?.getLong("producedFrames"))
                            .put("copiedFrameSequence", state.getBundle("observations")?.getLong("guestFrameSequence")))
                        assertEquals("Actual raw frame capture", "", error)
                        return target.absolutePath
                    }
                    fun awakePixels(state: Bundle): Boolean {
                        val io = state.getBundle("observations") ?: return false
                        return state.getString("state") == "running" && io.getBoolean("runtimeDiagnosticsAvailable") &&
                            io.getLong("engineReady") == 1L && io.getLong("displaySleeping") == 0L && io.getLong("backlightLevel") > 0L &&
                            io.getLong("guestFrameWidth") == 320L && io.getLong("guestFrameHeight") == 480L &&
                            io.getLong("guestFrameNonzeroRgbSamples") > 0L
                    }
                    fun coherentPixels(state: Bundle): Boolean {
                        if (!awakePixels(state)) return false
                        val io = requireNotNull(state.getBundle("observations"))
                        return io.getLong("guestFrameSequence") > 0L &&
                            io.getLong("guestFrameSequence") == io.getLong("producerFrameSequence") &&
                            io.getLong("guestFrameTimestampNs") > 0L &&
                            io.getLong("guestFrameTimestampNs") == io.getLong("producerFrameTimestampNs")
                    }
                    var wire: GuestTouchWireEvidence? = null
                    var gestureDisplay: GuestDisplay? = null
                    var admittedViewport: GuestUnlockReadiness.Viewport? = null
                    var gestureViewport: GuestUnlockReadiness.Viewport? = null
                    val viewIds = java.util.IdentityHashMap<GuestDisplay, Int>()
                    // Read the existing UI state without changing the pinned main APK.
                    // Missing/renamed getters fail closed; native wake alone is insufficient.
                    val uiStatus = MainActivity::class.java.getDeclaredMethod("getStatus").apply { isAccessible = true }
                    val uiAsleep = MainActivity::class.java.getDeclaredMethod("getDisplayAsleep").apply { isAccessible = true }
                    fun viewport(activity: MainActivity, state: Bundle): GuestUnlockReadiness.Viewport {
                        val display = requireNotNull(findDisplay(activity.window.decorView)) { "Production GuestDisplay missing" }
                        val io = requireNotNull(state.getBundle("observations"))
                        return GuestUnlockReadiness.Viewport(display, display.width, display.height,
                            state.getLong("displayInputGeneration"), io.getLong("surfaceGeneration"),
                            uiStatus.invoke(activity) == "running" && uiAsleep.invoke(activity) == false,
                            // viewport() runs inside onActivity on the app thread.
                            // ActivityScenario.state waits for idle and must only
                            // be queried from the instrumentation thread.
                            awakePixels(state) && activity.lifecycle.currentState == Lifecycle.State.RESUMED &&
                                display.holder.surface.isValid && display.hasWindowFocus() &&
                                display.isLaidOut && !display.isLayoutRequested)
                    }
                    fun viewportReceipt(value: GuestUnlockReadiness.Viewport): JSONObject {
                        val display = value.view as GuestDisplay
                        val id = viewIds[display] ?: (viewIds.size + 1).also { viewIds[display] = it }
                        return JSONObject().put("viewIdentity", id).put("viewIdentityHash", System.identityHashCode(display))
                            .put("width", value.width).put("height", value.height)
                            .put("inputGeneration", value.inputGeneration).put("surfaceGeneration", value.surfaceGeneration)
                            .put("awakeUi", value.awakeUi).put("usable", value.usable)
                            .put("viewInput", encode(display.inputObservations()))
                    }
                    var downTime = 0L
                    var upAttempted = false
                    fun send(action: Int, x: Float, state: Bundle) {
                        processIdentity()
                        val current = status("before-touch-$action")
                        onActivity { activity ->
                            val observed = viewport(activity, current)
                            val display = observed.view as GuestDisplay
                            val expected = gestureViewport ?: requireNotNull(admittedViewport)
                            val changed = !observed.ready() || !observed.sameViewport(expected)
                            val comparison = JSONObject().put("action", action).put("hostElapsedMs", SystemClock.elapsedRealtime())
                                .put("before", viewportReceipt(expected)).put("after", viewportReceipt(observed))
                                .put("sameViewObject", observed.view === expected.view).put("changed", changed)
                            timeline.put(JSONObject().put("viewportBeforeDispatch", comparison))
                            if (changed) { result.put("viewportChange", comparison); persist() }
                            check(!changed) { "Viewport changed during unlock; cancelled contacts must not be continued" }
                            if (gestureDisplay == null) {
                                check(action == MotionEvent.ACTION_DOWN) { "New viewport requires a new DOWN" }
                                gestureDisplay = display; gestureViewport = observed
                            }
                            check(display === gestureDisplay) { "Production GuestDisplay replaced during unlock" }
                            val viewport = GuestTestViewport(state, display.width, display.height)
                            val point = viewport.point(x, .92f)
                            val properties = arrayOf(MotionEvent.PointerProperties().apply { id = 7; toolType = MotionEvent.TOOL_TYPE_FINGER })
                            val coordinates = arrayOf(MotionEvent.PointerCoords().apply { this.x = point.first; y = point.second; pressure = 1f; size = .05f })
                            val time = SystemClock.uptimeMillis()
                            val event = MotionEvent.obtain(downTime, time, action, 1, properties, coordinates, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
                            try { assertTrue(display.dispatchTouchEvent(event)) } finally { event.recycle() }
                            val ownership = display.inputObservations()
                            events.put(JSONObject().put("action", action).put("pointerId", 7).put("eventUptimeMs", time)
                                .put("hostElapsedMs", SystemClock.elapsedRealtime()).put("guestX", x).put("guestY", .92f)
                                .put("viewXPx", point.first).put("viewYPx", point.second).put("viewport", viewport.receipt())
                                .put("displayRotation", display.display?.rotation).put("viewInput", encode(ownership)))
                            if (action != MotionEvent.ACTION_UP) {
                                assertFalse("Gesture rejected or cancelled", ownership.getBoolean("awaitingNewDown"))
                                assertArrayEquals(intArrayOf(7), ownership.getIntArray("androidContactIds"))
                            } else assertArrayEquals(intArrayOf(), ownership.getIntArray("androidContactIds"))
                        }
                    }
                    try {
                        var before = status("bound-existing")
                        if (before.getString("state") == "paused") {
                            require(arguments.getString("resumeExisting") == "true") { "Paused worker requires explicit resumeExisting" }
                            processIdentity()
                            worker.pause(false)
                            before = status("resume-requested")
                        }
                        val prior = requireNotNull(before.getBundle("observations"))
                        assertTrue("Native diagnostics required", prior.getBoolean("runtimeDiagnosticsAvailable"))
                        wire = if (initial.getBoolean("touchTraceConfigured")) GuestTouchWireEvidence(context.filesDir, initial, evidence) else null
                        result.put("touchWireTraceAlreadyConfigured", wire != null)
                        val wasSleeping = prior.getLong("displaySleeping") == 1L
                        var beforeCapturePath: String? = null
                        var awake: Bundle? = null
                        if (wasSleeping) {
                            result.put("readinessPath", "wake-new-producer").put("newProducerGrowthRequired", true)
                            val pulse = JSONObject().put("label", "power-wake").put("requestedHostHoldMs", 500)
                                .put("downHostElapsedMs", SystemClock.elapsedRealtime())
                            timeline.put(pulse)
                            processIdentity()
                            try { worker.button(1, true); pulse.put("downReturnedHostElapsedMs", SystemClock.elapsedRealtime()); Thread.sleep(500) }
                            finally { worker.button(1, false); pulse.put("upHostElapsedMs", SystemClock.elapsedRealtime()) }
                            val deadline = SystemClock.elapsedRealtime() + 15_000
                            do {
                                val observed = status("await-awake")
                                if (awakePixels(observed) && requireNotNull(observed.getBundle("observations")).getLong("producedFrames") > baselineFrames) {
                                    awake = observed; break
                                }
                                Thread.sleep(minOf(50, (deadline - SystemClock.elapsedRealtime()).coerceAtLeast(1)))
                            } while (SystemClock.elapsedRealtime() < deadline)
                        } else {
                            // An already-awake static Lock need not repaint. Pair its
                            // actual raw capture with the same latest producer/copy.
                            result.put("readinessPath", "already-awake-coherent-capture").put("newProducerGrowthRequired", false)
                            val started = SystemClock.elapsedRealtime()
                            val deadline = started + 500
                            var captureAttempt = 0
                            do {
                                val a = status("awake-coherence-before")
                                if (coherentPixels(a)) {
                                    val path = capture("before-swipe-coherence-${captureAttempt++}", a)
                                    val b = status("awake-coherence-after")
                                    val ai = requireNotNull(a.getBundle("observations"))
                                    val bi = requireNotNull(b.getBundle("observations"))
                                    if (SystemClock.elapsedRealtime() <= deadline && coherentPixels(b) &&
                                        ai.getLong("guestFrameSequence") == bi.getLong("guestFrameSequence") &&
                                        ai.getLong("guestFrameTimestampNs") == bi.getLong("guestFrameTimestampNs")) {
                                        awake = b; beforeCapturePath = path; break
                                    }
                                }
                                Thread.sleep(minOf(25, (deadline - SystemClock.elapsedRealtime()).coerceAtLeast(1)))
                            } while (SystemClock.elapsedRealtime() < deadline)
                            result.put("alreadyAwakeCoherenceElapsedMs", SystemClock.elapsedRealtime() - started)
                        }
                        val ready = requireNotNull(awake) { if (wasSleeping) "No awake/backlit/fresh nonzero producer frame within 15 seconds" else "No coherent awake raw capture within 500ms" }
                        val io = requireNotNull(ready.getBundle("observations"))
                        assertEquals(320L, io.getLong("guestFrameWidth")); assertEquals(480L, io.getLong("guestFrameHeight"))
                        assertEquals("Resolve existing native error before unlock diagnosis", "", ready.getString("error"))
                        result.put("awakeReadinessObserved", true).put("awakeObservedHostElapsedMs", SystemClock.elapsedRealtime())
                            .put("newProducerFrameObserved", io.getLong("producedFrames") > baselineFrames)
                            .put("beforeGesture", encode(ready))
                        if (beforeCapturePath == null) beforeCapturePath = capture("before-swipe", ready)
                        result.put("beforeGestureCapturePath", beforeCapturePath)
                        assertEquals(Lifecycle.State.RESUMED, scenario.state)
                        // MainActivity observes wake on its own 500ms poll. Wait for
                        // the actual UI and layout to settle before owning contacts.
                        val settlingBegan = SystemClock.elapsedRealtime()
                        val settling = GuestUnlockReadiness(settlingBegan)
                        var admittedReceipt: JSONObject? = null
                        do {
                            val observed = status("await-stable-awake-ui")
                            var stable = false
                            onActivity { activity ->
                                val candidate = viewport(activity, observed)
                                val now = SystemClock.elapsedRealtime()
                                timeline.put(JSONObject().put("viewportAdmission", viewportReceipt(candidate)).put("hostElapsedMs", now))
                                stable = settling.observe(now, candidate)
                                if (stable) { admittedViewport = candidate; admittedReceipt = viewportReceipt(candidate) }
                            }
                            if (stable) break
                            Thread.sleep(50)
                        } while (true)
                        result.put("stableAwakeUiAdmission", requireNotNull(admittedReceipt))
                            .put("stableAwakeUiAdmissionElapsedMs", SystemClock.elapsedRealtime() - settlingBegan)
                            .put("minimumStableAwakeUiMs", GuestUnlockReadiness.STABLE_MS)
                            .put("stableAwakeUiDeadlineMs", GuestUnlockReadiness.DEADLINE_MS)
                        persist()
                        downTime = SystemClock.uptimeMillis()
                        send(MotionEvent.ACTION_DOWN, .20f, ready)
                        for (step in 1..15) {
                            val at = downTime + step * 40L
                            Thread.sleep((at - SystemClock.uptimeMillis()).coerceAtLeast(0))
                            send(MotionEvent.ACTION_MOVE, .20f + .65f * step / 15f, ready)
                        }
                        upAttempted = true; send(MotionEvent.ACTION_UP, .85f, ready)
                        val upAt = SystemClock.elapsedRealtime()
                        result.put("gestureUpHostElapsedMs", upAt)
                        persist()
                        Thread.sleep((upAt + 300 - SystemClock.elapsedRealtime()).coerceAtLeast(0)); capture("after-300ms")
                        Thread.sleep((upAt + 1500 - SystemClock.elapsedRealtime()).coerceAtLeast(0)); capture("after-1500ms")
                        wire?.drain(events.length() - 1)
                        val terminal = status("before-activity-close")
                        assertEquals("Native gesture error", "", terminal.getString("error"))
                        result.put("terminalWorker", encode(terminal)).put("hostExerciseCompleted", true)
                    } finally {
                        if (gestureDisplay != null && !upAttempted) {
                            try { upAttempted = true; send(MotionEvent.ACTION_UP, .85f, status("release-after-failure")) }
                            catch (failure: Throwable) { result.put("upReleaseFailure", failure.toString()) }
                        }
                        try { processIdentity(); worker.cancelInput(); wire?.drain(events.length() - 1); result.put("cleanupWorker", encode(worker.status())) }
                        catch (failure: Exception) { result.put("cancelFailure", failure.toString()) }
                        persist()
                    }
                }
            }
        } catch (failure: Throwable) { result.put("failure", failure.toString()); throw failure }
        finally { persist() }
    }
}
