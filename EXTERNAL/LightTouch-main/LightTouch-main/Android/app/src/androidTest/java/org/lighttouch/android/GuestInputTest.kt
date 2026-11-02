package org.lighttouch.android

import android.content.*
import android.hardware.*
import android.os.*
import android.view.*
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
import java.util.concurrent.atomic.AtomicInteger

/** Private, opt-in real-guest harness. Host delivery is never guest gesture proof. */
@RunWith(AndroidJUnit4::class)
class GuestInputTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val arguments = InstrumentationRegistry.getArguments()
    private fun findDisplay(view: View): GuestDisplay? {
        if (view is GuestDisplay) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) findDisplay(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun guestOperation(worker: IEngine, operation: Int, argument: String): String {
        val queued = worker.guestRequest(operation, argument, 10_000)
        assertEquals(queued.toString(), "", queued.getString("error"))
        val id = queued.getLong("requestId")
        val deadline = SystemClock.elapsedRealtime() + 11_000
        try {
            while (SystemClock.elapsedRealtime() < deadline) {
                val result = worker.guestPoll(id)
                assertEquals(result.toString(), "", result.getString("error"))
                when (result.getInt("state")) {
                    2 -> { assertEquals(result.toString(), 0, result.getInt("guestStatus")); return result.getString("output") ?: "" }
                    3, 4, 5 -> error("Guest operation failed: $result")
                }
                Thread.sleep(50)
            }
            error("Guest operation timed out; mutations are not retried")
        } finally { worker.guestCancel(id) }
    }
    private fun withGuest(action: (ActivityScenario<MainActivity>, IEngine, File) -> Unit) {
        assumeTrue("Requires legitimate fixture and visual guest workload", arguments.getString("exerciseGuest") == "true")
        val context = instrumentation.targetContext
        val evidence = File(context.filesDir, "input-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val outcome = JSONObject().put("hostExerciseCompleted", false).put("guestAcceptancePassed", false)
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is IntArray -> put(key, JSONArray(value.toList()))
                is LongArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value)
            }
        }
        try { ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            var engine: IEngine? = null
            val connected = CountDownLatch(1)
            val connection = object : ServiceConnection {
                override fun onServiceConnected(name: ComponentName, binder: IBinder) { engine = IEngine.Stub.asInterface(binder); connected.countDown() }
                override fun onServiceDisconnected(name: ComponentName) = Unit
            }
            assertTrue(context.bindService(Intent(context, EngineService::class.java), connection, Context.BIND_AUTO_CREATE))
            try {
                assertTrue(connected.await(10, TimeUnit.SECONDS))
                val worker = requireNotNull(engine)
                val initial = worker.status()
                outcome.put("initialWorker", encode(initial))
                File(evidence, "result.json").writeText(outcome.toString(2))
                val state = initial.getString("state")
                if (state == "stopped") {
                    if (arguments.getString("touchWireTrace") == "true") {
                        assertFalse("Trace opt-in requires an unused worker", initial.getBoolean("sessionStarted"))
                        assertEquals("Fixed native trace admission", "", worker.debugEnableTouchTrace())
                    }
                    scenario.onActivity { it.startForegroundService(Intent(it, EngineService::class.java)) }
                    val started = worker.start(requireNotNull(worker.records().firstOrNull { it.getBoolean("selected") }?.getString("id")) { "Import and select the legitimate private device first" })
                    assertEquals(started.toString(), "", started.getString("error"))
                } else if (state == "paused") worker.pause(false)
                val deadline = SystemClock.elapsedRealtime() + 120_000
                while (worker.status().getString("state") != "running" && SystemClock.elapsedRealtime() < deadline) Thread.sleep(100)
                assertEquals(worker.status().toString(), "running", worker.status().getString("state"))
                if (arguments.getString("touchWireTrace") == "true") {
                    assertTrue("Existing worker must have configured trace before start", worker.status().getBoolean("touchTraceConfigured"))
                    assertTrue("Trace cannot be changed after start", worker.debugEnableTouchTrace().isNotEmpty())
                }
                // Root owns this device while the harness runs and opens the chosen
                // built-in guest app. State=running alone does not prove Home.
                val seconds = arguments.getString("settleSeconds", "60").toInt().coerceIn(0, 300)
                Thread.sleep(seconds * 1000L)
                outcome.put("beforeGuestOperation", encode(worker.status()))
                File(evidence, "result.json").writeText(outcome.toString(2))
                arguments.getString("guestBundle")?.let { id ->
                    guestOperation(worker, if (arguments.getString("gyroProbe") == "true" && arguments.getString("experimentalMotion") == "true") 5 else 2, id)
                    Thread.sleep(1000)
                    val frontmost = guestOperation(worker, 1, "").lineSequence().firstOrNull()
                    assertEquals("Actual guest frontmost workload", id, frontmost)
                }
                action(scenario, worker, evidence)
                outcome.put("terminalWorker", encode(worker.status())).put("hostExerciseCompleted", true)
            } finally { context.unbindService(connection) }
        } } catch (failure: Throwable) { outcome.put("failure", failure.toString()); throw failure }
        finally { File(evidence, "result.json").writeText(outcome.toString(2)) }
    }
    @Test fun stableTwoPointerTrajectoriesUseProductionDisplayPath() = withGuest { scenario, worker, evidence ->
        val trace = JSONArray()
        val ready = worker.status()
        assertEquals("Existing worker error must be resolved before gesture acceptance: $ready", "", ready.getString("error"))
        val postGestureSettleSeconds = arguments.getString("postGestureSettleSeconds", "0").toInt().coerceIn(0, 30)
        trace.put(JSONObject().put("postGestureSettleSeconds", postGestureSettleSeconds).put("guestAcceptancePassed", false))
        val wire = if (arguments.getString("touchWireTrace") == "true") GuestTouchWireEvidence(instrumentation.targetContext.filesDir, worker.status(), evidence) else null
        var touchIndex = 0
        val selected = arguments.getString("gesture", "all")
        var exerciseFailure: Throwable? = null
        try {
        for (kind in listOf("pinch", "cross", "cancel").filter { selected == "all" || selected == it }) {
            val down = SystemClock.uptimeMillis()
            var eventIndex = 0
            fun send(action: Int, contacts: List<Triple<Int, Float, Float>>) {
                // Fetch Binder observations off the activity thread. Fractions describe
                // the guest image; landscape View margins are deliberately excluded.
                val status = worker.status()
                scenario.onActivity { activity ->
                    val display = requireNotNull(findDisplay(activity.window.decorView)) { "Production GuestDisplay missing" }
                    val viewport = GuestTestViewport(status, display.width, display.height)
                    require(contacts.all { it.first in 0..31 }) { "Android pointer IDs must fit the platform range" }
                    val properties = contacts.map { MotionEvent.PointerProperties().apply { id = it.first; toolType = MotionEvent.TOOL_TYPE_FINGER } }.toTypedArray()
                    val points = contacts.map { viewport.point(it.second, it.third) }
                    val coordinates = points.map { MotionEvent.PointerCoords().apply { x = it.first; y = it.second; pressure = 1f; size = .05f } }.toTypedArray()
                    val time = SystemClock.uptimeMillis()
                    val event = MotionEvent.obtain(down, time, action, contacts.size, properties, coordinates, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
                    try { assertTrue(display.dispatchTouchEvent(event)) } finally { event.recycle() }
                    val entries = JSONArray()
                    for ((index, contact) in contacts.withIndex()) entries.put(JSONObject().put("id", contact.first)
                        .put("guestX", contact.second).put("guestY", contact.third).put("viewXPx", points[index].first).put("viewYPx", points[index].second))
                    trace.put(JSONObject().put("gesture", kind).put("index", eventIndex++).put("touchIndex", touchIndex++).put("action", action).put("uptimeMs", time)
                        .put("viewport", viewport.receipt()).put("displayRotation", display.display.rotation).put("contacts", entries))
                }
                File(evidence, "touch.json").writeText(JSONObject().put("events", trace).put("guestOutcomeRequiresReview", true).put("guestAcceptancePassed", false).toString(2))
                Thread.sleep(20)
                wire?.drain(touchIndex - 1)
            }
            fun screenshot(label: String) {
                val target = File(instrumentation.targetContext.filesDir, "screenshots/input-$kind-$label-${System.currentTimeMillis()}.png")
                assertEquals("", worker.screenshot(target.absolutePath))
                trace.put(JSONObject().put("screenshot", target.absolutePath))
                File(evidence, "touch.json").writeText(JSONObject().put("events", trace).put("guestOutcomeRequiresReview", true).put("guestAcceptancePassed", false).toString(2))
            }
            screenshot("before")
            send(MotionEvent.ACTION_DOWN, listOf(Triple(7, .42f, .50f)))
            send(MotionEvent.ACTION_POINTER_DOWN or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), listOf(Triple(7, .42f, .50f), Triple(23, .58f, .50f)))
            for (step in 1..40) {
                val t = step / 40f
                val a = if (kind == "pinch") .42f - .16f*t else .42f + .16f*t
                val b = if (kind == "pinch") .58f + .16f*t else .58f - .16f*t
                send(MotionEvent.ACTION_MOVE, listOf(Triple(7, a, .50f), Triple(23, b, .50f)))
            }
            screenshot("two-held")
            if (kind == "cancel") send(MotionEvent.ACTION_CANCEL, listOf(Triple(7, .58f, .50f), Triple(23, .42f, .50f)))
            else {
                val a = if (kind == "pinch") .26f else .58f
                val b = if (kind == "pinch") .74f else .42f
                send(MotionEvent.ACTION_POINTER_UP, listOf(Triple(7, a, .50f), Triple(23, b, .50f)))
                for (step in 1..8) send(MotionEvent.ACTION_MOVE, listOf(Triple(23, b, .50f + step * .01f)))
                screenshot("independent-lift")
                send(MotionEvent.ACTION_UP, listOf(Triple(23, b, .58f)))
            }
            screenshot("after")
            if (postGestureSettleSeconds > 0) {
                val waitingAt = SystemClock.elapsedRealtime()
                Thread.sleep(postGestureSettleSeconds * 1000L)
                trace.put(JSONObject().put("gesture", kind).put("settleStartedHostMs", waitingAt)
                    .put("settleCompletedHostMs", SystemClock.elapsedRealtime()).put("guestOutcomeRequiresReview", true))
                wire?.drain(touchIndex - 1)
                screenshot("after-settled") // Before ActivityScenario.close/background pause.
            }
            Thread.sleep(200)
            val status = worker.status()
            assertEquals("Production path rejected input: $status", "", status.getString("error"))
        }
        if (wire != null) {
            Thread.sleep(1000); wire.drain(touchIndex - 1)
            assertTrue("No actual guest FRAME_READ with a pending frame in the retained gesture interval", wire.collectedFrameCount() > 0)
            assertTrue("No serialized nonempty contact records in retained wire interval", wire.serializedContactCount() > 0)
        }
        File(evidence, "touch.json").writeText(JSONObject().put("events", trace).put("workerPid", worker.status().getInt("pid"))
            .put("guestOutcomeRequiresReview", true).put("guestAcceptancePassed", false).toString(2))
        } catch (failure: Throwable) { exerciseFailure = failure; throw failure }
        finally {
            // A screenshot or dispatch failure while two contacts are held must
            // still request release. No successful cleanup erases the failure.
            var cleanupFailure: Throwable? = null
            try { worker.cancelInput() } catch (failure: Throwable) { cleanupFailure = failure }
            try { wire?.drain(touchIndex - 1) } catch (failure: Throwable) {
                if (cleanupFailure == null) cleanupFailure = failure else cleanupFailure.addSuppressed(failure)
            }
            cleanupFailure?.let { failure ->
                File(evidence, "cleanup-error.txt").writeText(failure.toString())
                if (exerciseFailure == null) throw failure else exerciseFailure?.addSuppressed(failure)
            }
        }
    }
    @Test fun deferredPhysicalMotionRecordingHarness() {
        assumeTrue("Motion is deferred/unqualified by user scope D12", arguments.getString("experimentalMotion") == "true")
        withGuest { _, worker, evidence ->
        val context = instrumentation.targetContext
        val manager = context.getSystemService(SensorManager::class.java)
        val gyro = requireNotNull(manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)) { "Physical calibrated gyro unavailable" }
        val acceleration = requireNotNull(manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER))
        val count = AtomicInteger(0)
        val samples = StringBuilder("kind,boottime_ns,x,y,z,accuracy\n")
        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                synchronized(samples) { samples.append("${event.sensor.type},${event.timestamp},${event.values[0]},${event.values[1]},${event.values[2]},${event.accuracy}\n") }
                if (event.sensor.type == Sensor.TYPE_GYROSCOPE) count.incrementAndGet()
            }
            override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) = Unit
        }
        val thread = HandlerThread("physical-motion-evidence").apply { start() }
        try {
            assertTrue(manager.registerListener(listener, gyro, 20_000, Handler(thread.looper)))
            assertTrue(manager.registerListener(listener, acceleration, 20_000, Handler(thread.looper)))
            // No synthetic motion injection. Root/user physically rotates the phone.
            val seconds = arguments.getString("motionSeconds", "30").toInt().coerceIn(1, 180)
            Thread.sleep(seconds * 1000L)
            assertTrue("No actual gyro samples", count.get() > 0)
            val target = File(context.filesDir, "screenshots/motion-${System.currentTimeMillis()}.png")
            assertEquals("", worker.screenshot(target.absolutePath))
            synchronized(samples) { File(evidence, "physical-sensors.csv").writeText(samples.toString()) }
            File(evidence, "motion.json").writeText(JSONObject().put("gyro", gyro.name).put("accelerometer", acceleration.name)
                .put("gyroUnits", "rad/s").put("accelerationUnits", "m/s^2").put("guestStatus", worker.status().getString("detail"))
                .put("screenshot", target.absolutePath).put("guestResponseRequiresReview", true).put("guestAcceptancePassed", false).toString(2))
        } finally { manager.unregisterListener(listener); thread.quitSafely() }
    }
    }
}
