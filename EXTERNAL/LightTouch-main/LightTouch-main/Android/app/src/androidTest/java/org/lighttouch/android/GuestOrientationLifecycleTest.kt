package org.lighttouch.android

import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.graphics.Bitmap
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityNodeInfo
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
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream

/** Opt-in real guest exercise, with host observations kept separate from visual proof. */
@RunWith(AndroidJUnit4::class)
class GuestOrientationLifecycleTest {
    @Test fun recreateRotateCancelReuseAndBalanceProductionControls() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Root must boot the legitimate guest first", arguments.getString("exerciseOrientationLifecycle") == "true")
        val context = instrumentation.targetContext
        val evidence = File(context.filesDir, "orientation-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("hostChecksPassed", false).put("guestAcceptancePassed", false)
            .put("visibleGuestVerified", false).put("audiblePlaybackVerified", false)
        result.put("powerTransitions", JSONArray())
        val observations = JSONArray()
        val events = JSONArray()
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
            File(evidence, "observations.json").writeText(observations.toString(2))
            File(evidence, "events.json").writeText(events.toString(2))
            File(evidence, "result.json").writeText(result.toString(2))
        }
        fun findDisplay(view: View): GuestDisplay? {
            if (view is GuestDisplay) return view
            if (view is ViewGroup) for (index in 0 until view.childCount) findDisplay(view.getChildAt(index))?.let { return it }
            return null
        }
        try {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                var originalOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                scenario.onActivity { originalOrientation = it.requestedOrientation }
                AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                    val worker = binding.get()
                    val initial = worker.status()
                    assertTrue("A real guest must already be running", initial.getString("state") in listOf("running", "paused"))
                    val wire = if (arguments.getString("touchWireTrace") == "true") GuestTouchWireEvidence(context.filesDir, initial, evidence) else null
                    val workerPid = initial.getInt("pid")
                    fun workerStartTicks(): Long {
                        val stat = File("/proc/$workerPid/stat").readText()
                        return stat.substring(stat.lastIndexOf(')') + 1).trim().split(Regex("\\s+"))[19].toLong()
                    }
                    val workerStart = workerStartTicks()
                    assertEquals(Process.myUid(), initial.getInt("uid")); assertNotEquals(Process.myPid(), workerPid)
                    result.put("workerPid", workerPid).put("appUid", Process.myUid()).put("originalRequestedOrientation", originalOrientation)
                    fun awaitState(expected: String) {
                        val deadline = SystemClock.elapsedRealtime() + 15_000
                        do {
                            val status = worker.status()
                            assertEquals("Worker changed during lifecycle", workerPid, status.getInt("pid"))
                            check(status.getString("state") != "failed") { status.toString() }
                            if (status.getString("state") == expected) return
                            Thread.sleep(100)
                        } while (SystemClock.elapsedRealtime() < deadline)
                        fail("Expected $expected, actual ${worker.status()}")
                    }
                    fun readyDisplay(expectedOrientation: Int? = null): GuestDisplay {
                        val deadline = SystemClock.elapsedRealtime() + 15_000
                        do {
                            var display: GuestDisplay? = null
                            scenario.onActivity { activity ->
                                if (expectedOrientation == null || activity.resources.configuration.orientation == expectedOrientation) {
                                    findDisplay(activity.window.decorView)?.takeIf { it.width > 0 && it.height > 0 && it.holder.surface.isValid }?.let { display = it }
                                }
                            }
                            display?.let { return it }
                            Thread.sleep(100)
                        } while (SystemClock.elapsedRealtime() < deadline)
                        error("Production Surface/orientation was not ready; OS orientation policy may have rejected this request")
                    }
                    fun snapshot(label: String, screenshot: Boolean = true): Bundle {
                        wire?.drain(events.length() - 1)
                        val status = worker.status()
                        assertEquals("Worker identity", workerPid, status.getInt("pid"))
                        assertEquals("Actionable worker error", "", status.getString("error"))
                        val item = JSONObject().put("label", label).put("hostElapsedMs", SystemClock.elapsedRealtime()).put("status", encode(status))
                        scenario.onActivity { activity ->
                            val display = requireNotNull(findDisplay(activity.window.decorView))
                            item.put("activityIdentity", System.identityHashCode(activity)).put("requestedOrientation", activity.requestedOrientation)
                                .put("configurationOrientation", activity.resources.configuration.orientation).put("displayRotation", display.display.rotation)
                                .put("viewWidth", display.width).put("viewHeight", display.height).put("surfaceValid", display.holder.surface.isValid)
                                .put("androidInput", encode(display.inputObservations()))
                        }
                        observations.put(item); persist()
                        if (screenshot) {
                            val path = File(context.filesDir, "screenshots/${evidence.name}/$label.png")
                            item.put("captureBeginElapsedMs", SystemClock.elapsedRealtime()); persist()
                            val captureError = worker.screenshot(path.absolutePath)
                            item.put("captureEndElapsedMs", SystemClock.elapsedRealtime()).put("captureError", captureError)
                                .put("screenshot", path.absolutePath); persist()
                            assertEquals("Actual frame screenshot", "", captureError)
                        }
                        return requireNotNull(status.getBundle("observations"))
                    }
                    var gestureDown = SystemClock.uptimeMillis()
                    fun sendContacts(action: Int, contacts: List<Triple<Int, Float, Float>>) {
                        if ((action and MotionEvent.ACTION_MASK) == MotionEvent.ACTION_DOWN) gestureDown = SystemClock.uptimeMillis()
                        val status = worker.status()
                        scenario.onActivity { activity ->
                            val display = requireNotNull(findDisplay(activity.window.decorView))
                            val viewport = GuestTestViewport(status, display.width, display.height)
                            require(contacts.all { it.first in 0..31 })
                            val points = contacts.map { viewport.point(it.second, it.third) }
                            val properties = contacts.map { MotionEvent.PointerProperties().apply { id = it.first; toolType = MotionEvent.TOOL_TYPE_FINGER } }.toTypedArray()
                            val coordinates = points.map { MotionEvent.PointerCoords().apply { x = it.first; y = it.second; pressure = 1f; size = .05f } }.toTypedArray()
                            val eventTime = SystemClock.uptimeMillis()
                            val event = MotionEvent.obtain(gestureDown, eventTime, action, contacts.size, properties, coordinates, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
                            try { assertTrue(display.dispatchTouchEvent(event)) } finally { event.recycle() }
                            val entries = JSONArray()
                            for ((index, contact) in contacts.withIndex()) entries.put(JSONObject().put("id", contact.first).put("guestX", contact.second).put("guestY", contact.third)
                                .put("viewXPx", points[index].first).put("viewYPx", points[index].second))
                            events.put(JSONObject().put("action", action).put("contacts", entries).put("viewport", viewport.receipt())
                                .put("uptimeMs", eventTime).put("displayRotation", display.display.rotation).put("input", encode(display.inputObservations())))
                        }
                        persist(); Thread.sleep(30); wire?.drain(events.length() - 1)
                    }
                    fun send(action: Int, id: Int = 7, x: Float = .5f, y: Float = .5f) = sendContacts(action, listOf(Triple(id, x, y)))
                    fun holdPair() {
                        send(MotionEvent.ACTION_DOWN, 7, .45f)
                        sendContacts(MotionEvent.ACTION_POINTER_DOWN or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), listOf(Triple(7, .45f, .5f), Triple(23, .55f, .5f)))
                        scenario.onActivity { assertArrayEquals("Both Android contacts must be held before interruption", intArrayOf(7, 23), requireNotNull(findDisplay(it.window.decorView)).inputObservations().getIntArray("androidContactIds")) }
                    }
                    fun clickControl(text: String, sharedDeadline: Long = Long.MAX_VALUE) {
                        val deadline = GuestPowerTransition.controlDeadline(SystemClock.elapsedRealtime(), sharedDeadline)
                        val attempts = JSONArray()
                        val lookup = JSONObject().put("text", text).put("deadlineElapsedMs", deadline)
                            .put("nodeLimit", 2048).put("depthLimit", 64).put("attempts", attempts)
                            .put("clicked", false)
                        val lookups = result.optJSONArray("semanticControlLookups")
                            ?: JSONArray().also { result.put("semanticControlLookups", it) }
                        check(lookups.length() < 32) { "Semantic control lookup receipt limit" }
                        lookups.put(lookup)
                        fun retainFailureEvidence(reason: String) {
                            val failure = JSONObject().put("reason", reason).put("elapsedMs", SystemClock.elapsedRealtime())
                                .put("beforeScenarioClose", true).put("atomicWindowStatusBindingClaimed", false)
                            lookup.put("failureEvidence", failure)
                            // Capture the actual Android window before a potentially blocked
                            // worker read and before Scenario.close returns to Launcher.
                            try {
                                val image = requireNotNull(instrumentation.uiAutomation.takeScreenshot()) { "Android screenshot unavailable" }
                                try {
                                    require(image.width > 0 && image.height > 0 && image.width.toLong() * image.height <= 8_388_608)
                                    val path = File(evidence, "control-${lookups.length()}-failure-android.png")
                                    val partial = File(evidence, path.name + ".partial")
                                    var count = 0L
                                    FileOutputStream(partial).use { file ->
                                        val limited = object : OutputStream() {
                                            override fun write(value: Int) {
                                                if (++count > 8_388_608) throw IOException("Android PNG exceeds evidence limit")
                                                file.write(value)
                                            }
                                            override fun write(bytes: ByteArray, offset: Int, length: Int) {
                                                if (length.toLong() > 8_388_608 - count) throw IOException("Android PNG exceeds evidence limit")
                                                file.write(bytes, offset, length); count += length
                                            }
                                        }
                                        check(image.compress(Bitmap.CompressFormat.PNG, 100, limited)) { "Android PNG encoding failed" }
                                    }
                                    check(partial.renameTo(path)) { "Android PNG publication failed" }
                                    failure.put("androidScreenshot", path.absolutePath).put("pngBytes", count)
                                        .put("width", image.width).put("height", image.height)
                                } finally { image.recycle() }
                            } catch (captureFailure: Exception) { failure.put("androidScreenshotFailure", captureFailure.toString()) }
                            persist()
                            try {
                                failure.put("workerReadBeginElapsedMs", SystemClock.elapsedRealtime())
                                failure.put("workerStatus", encode(worker.status()))
                                failure.put("workerReadEndElapsedMs", SystemClock.elapsedRealtime())
                            } catch (statusFailure: Exception) { failure.put("workerStatusFailure", statusFailure.toString()) }
                            persist()
                        }
                        var attemptCount = 0
                        do {
                            val ownedNodes = java.util.IdentityHashMap<AccessibilityNodeInfo, Boolean>()
                            fun retain(node: AccessibilityNodeInfo?): AccessibilityNodeInfo? = node?.also { ownedNodes[it] = true }
                            val attempt = JSONObject().put("elapsedMs", SystemClock.elapsedRealtime())
                            attemptCount++; lookup.put("attemptCount", attemptCount)
                            if (attempts.length() == 8) attempts.remove(0)
                            attempts.put(attempt)
                            try {
                                val cacheCleared = if (Build.VERSION.SDK_INT >= 34) instrumentation.uiAutomation.clearCache() else null
                                val refreshNodes = cacheCleared != true
                                attempt.put("sdkInt", Build.VERSION.SDK_INT).put("cacheClearSupported", Build.VERSION.SDK_INT >= 34)
                                    .put("cacheClearReturned", cacheCleared ?: JSONObject.NULL).put("refreshNodes", refreshNodes)
                                val root = retain(instrumentation.uiAutomation.rootInActiveWindow)
                                if (refreshNodes && root != null) attempt.put("rootRefreshReturned", root.refresh())
                                val window = root?.windowId
                                attempt.put("rootPackage", root?.packageName?.toString() ?: JSONObject.NULL)
                                    .put("rootWindowId", window ?: JSONObject.NULL)
                                val controls = mutableListOf<AccessibilityNodeInfo>()
                                val nodeTexts = JSONArray(); val stateCandidates = JSONArray()
                                attempt.put("ownNodeTexts", nodeTexts).put("statusControlCandidates", stateCandidates)
                                var labels = 0; var visited = 0
                                var refreshed = 0; var refreshRejected = 0; var refreshedTextChanges = 0
                                if (root != null && root.packageName?.toString() == context.packageName) {
                                    // Compose 1.7.8's virtual provider inherits a null text-search
                                    // implementation. Traverse its actual virtual children instead.
                                    val pending = java.util.ArrayDeque<Pair<AccessibilityNodeInfo, Int>>()
                                    pending.add(root to 0)
                                    while (pending.isNotEmpty() && SystemClock.elapsedRealtime() < deadline) {
                                        val (node, depth) = pending.removeLast()
                                        check(++visited <= 2048 && depth <= 64) { "Semantic tree traversal limit" }
                                        if (refreshNodes) {
                                            val cachedText = node.text?.toString()
                                            if (!node.refresh()) { refreshRejected++; continue }
                                            refreshed++
                                            if (cachedText != node.text?.toString()) refreshedTextChanges++
                                        }
                                        if (node.packageName?.toString() != context.packageName || node.windowId != window) continue
                                        val label = node.text?.toString().orEmpty()
                                        val id = node.viewIdResourceName.orEmpty()
                                        if (label.isNotEmpty() && nodeTexts.length() < 64) nodeTexts.put(JSONObject()
                                            .put("text", label.take(256)).put("resourceId", id.take(128))
                                            .put("visible", node.isVisibleToUser).put("enabled", node.isEnabled).put("clickable", node.isClickable))
                                        if ((label in listOf("Pause", "Resume", "running", "paused") || id.endsWith("session_status")) && stateCandidates.length() < 8)
                                            stateCandidates.put(JSONObject().put("text", label.take(256)).put("resourceId", id.take(128))
                                                .put("visible", node.isVisibleToUser).put("enabled", node.isEnabled).put("clickable", node.isClickable))
                                        if (node.text?.toString() == text && node.isVisibleToUser) {
                                            labels++
                                            var ancestor: AccessibilityNodeInfo? = node
                                            var distance = 0
                                            while (ancestor != null && distance++ <= 64 && SystemClock.elapsedRealtime() < deadline &&
                                                ancestor.packageName?.toString() == context.packageName && ancestor.windowId == window) {
                                                if (refreshNodes && !ancestor.refresh()) break
                                                if (ancestor.isClickable && ancestor.isEnabled && ancestor.isVisibleToUser) {
                                                    controls.add(ancestor); break
                                                }
                                                ancestor = retain(ancestor.parent)
                                            }
                                        }
                                        check(node.childCount <= 2048) { "Semantic child-count limit" }
                                        for (index in 0 until node.childCount) {
                                            if (SystemClock.elapsedRealtime() >= deadline) break
                                            check(visited + pending.size < 2048) { "Semantic node budget exhausted" }
                                            retain(node.getChild(index))?.let { pending.add(it to depth + 1) }
                                        }
                                    }
                                }
                                val unique = controls.distinct()
                                attempt.put("visitedNodes", visited).put("exactVisibleLabels", labels).put("enabledClickableCandidates", unique.size)
                                    .put("refreshedNodes", refreshed).put("refreshRejectedNodes", refreshRejected).put("refreshedTextChanges", refreshedTextChanges)
                                check(unique.size <= 1) { "Ambiguous semantic production control: $text" }
                                val current = retain(instrumentation.uiAutomation.rootInActiveWindow)
                                val currentFresh = !refreshNodes || current?.refresh() == true
                                if (refreshNodes) attempt.put("currentRootRefreshReturned", currentFresh)
                                val sameWindow = currentFresh && root != null && current?.windowId == window && current?.packageName?.toString() == context.packageName
                                attempt.put("currentRootPackage", current?.packageName?.toString() ?: JSONObject.NULL)
                                    .put("currentRootWindowId", current?.windowId ?: JSONObject.NULL).put("sameActiveWindow", sameWindow)
                                val clicked = SystemClock.elapsedRealtime() < deadline && sameWindow &&
                                    unique.singleOrNull()?.performAction(AccessibilityNodeInfo.ACTION_CLICK) == true
                                attempt.put("actionReturnedTrue", clicked)
                                if (clicked) { lookup.put("clicked", true); persist(); return }
                            } catch (failure: Throwable) {
                                attempt.put("failure", failure.toString()); retainFailureEvidence(failure.toString()); throw failure
                            } finally {
                                @Suppress("DEPRECATION")
                                for (node in ownedNodes.keys) node.recycle()
                            }
                            Thread.sleep(minOf(100, maxOf(0, deadline - SystemClock.elapsedRealtime())))
                        } while (SystemClock.elapsedRealtime() < deadline)
                        retainFailureEvidence("Semantic production control unavailable: $text")
                        error("Semantic production control unavailable: $text")
                    }
                    fun powerTransition(label: String, baselineFrames: Long, waking: Boolean): Bundle {
                        // Include semantic control lookup/click time in this one original budget.
                        val began = SystemClock.elapsedRealtime()
                        val gate = GuestPowerTransition(began, workerPid, initial.getInt("uid"), workerStart, baselineFrames, waking)
                        val transition = JSONObject().put("label", label).put("beganElapsedMs", began)
                            .put("deadlineElapsedMs", gate.deadline).put("baselineProducedFrames", baselineFrames)
                            .put("waking", waking).put("nativePredicateObserved", false)
                            .put("visibleGuestVerified", false).put("samples", JSONArray())
                        result.getJSONArray("powerTransitions").put(transition); persist()
                        try {
                            fun observePower(phase: String): Pair<Bundle, Boolean> {
                                val readBegin = SystemClock.elapsedRealtime()
                                val status = worker.status()
                                val start = workerStartTicks()
                                val readEnd = SystemClock.elapsedRealtime()
                                val sample = JSONObject().put("phase", phase).put("readBeginElapsedMs", readBegin).put("readEndElapsedMs", readEnd)
                                    .put("workerStartTicks", start).put("status", encode(status))
                                transition.getJSONArray("samples").put(sample); persist()
                                val o = requireNotNull(status.getBundle("observations")) { "Missing native power observations" }
                                val required = listOf("runtimeDiagnosticsAvailable", "displaySleeping", "backlightLevel", "producedFrames",
                                    "guestFrameSamplePixels", "guestFrameNonzeroRgbSamples", "guestFrameRgbSampleSum")
                                check(required.all { o.containsKey(it) } && status.containsKey("nativePollSnapshotValid")) {
                                    "Missing required native power observation keys"
                                }
                                val decision = SystemClock.elapsedRealtime()
                                sample.put("decisionElapsedMs", decision)
                                val matched = gate.observe(decision, status.getInt("pid"), status.getInt("uid"), start,
                                        status.getString("state"), status.getString("error"), status.getBoolean("nativePollSnapshotValid"),
                                        o.getBoolean("runtimeDiagnosticsAvailable"), o.getLong("displaySleeping"), o.getLong("backlightLevel"),
                                        o.getLong("producedFrames"), o.getLong("guestFrameSamplePixels"),
                                        o.getLong("guestFrameNonzeroRgbSamples"), o.getLong("guestFrameRgbSampleSum"))
                                persist()
                                return Pair(o, matched)
                            }
                            val preClick = observePower("before-click").first
                            assertEquals("Unexpected display state before isolated Power press", if (waking) 1L else 0L, preClick.getLong("displaySleeping"))
                            if (waking) assertEquals("Wake must start with backlight off", 0L, preClick.getLong("backlightLevel"))
                            else assertTrue("Sleep must start with backlight on", preClick.getLong("backlightLevel") > 0)
                            check(SystemClock.elapsedRealtime() < gate.deadline) { "Power transition admission deadline expired" }
                            clickControl("Power", gate.deadline)
                            while (true) {
                                val (o, matched) = observePower("after-click")
                                if (matched) {
                                    transition.put("nativePredicateObserved", true).put("observedElapsedMs", SystemClock.elapsedRealtime()); persist()
                                    // This copied frame still requires independent human visual review.
                                    snapshot(label)
                                    return o
                                }
                                Thread.sleep(minOf(100, (gate.deadline - SystemClock.elapsedRealtime()).coerceAtLeast(1)))
                            }
                        } catch (failure: Throwable) {
                            transition.put("failure", failure.toString()).put("failedElapsedMs", SystemClock.elapsedRealtime()); persist()
                            // Keep a real timeout/failure frame, without extending the transition or retrying input.
                            try { snapshot("$label-failed") }
                            catch (captureFailure: Throwable) { transition.put("failureCaptureError", captureFailure.toString()); persist() }
                            throw failure
                        }
                    }
                    fun resume() { worker.pause(false); awaitState("running"); readyDisplay(); Thread.sleep(300) }
                    fun reuseAndRelease(label: String) {
                        var before = 0L
                        scenario.onActivity { before = requireNotNull(findDisplay(it.window.decorView)).inputObservations().getLong("forwardedBatches") }
                        send(MotionEvent.ACTION_MOVE, 7)
                        scenario.onActivity {
                            val display = requireNotNull(findDisplay(it.window.decorView))
                            assertEquals("Old MOVE forwarded without fresh DOWN", before, display.inputObservations().getLong("forwardedBatches"))
                        }
                        send(MotionEvent.ACTION_DOWN, 7, .48f); send(MotionEvent.ACTION_MOVE, 7, .52f); send(MotionEvent.ACTION_UP, 7, .52f)
                        send(MotionEvent.ACTION_DOWN, 23); send(MotionEvent.ACTION_CANCEL, 23)
                        send(MotionEvent.ACTION_MOVE, 23)
                        send(MotionEvent.ACTION_DOWN, 23); send(MotionEvent.ACTION_UP, 23)
                        holdPair()
                        sendContacts(MotionEvent.ACTION_MOVE, listOf(Triple(7, .4f, .5f), Triple(23, .6f, .5f)))
                        sendContacts(MotionEvent.ACTION_POINTER_UP, listOf(Triple(7, .4f, .5f), Triple(23, .6f, .5f)))
                        send(MotionEvent.ACTION_MOVE, 23, .62f); send(MotionEvent.ACTION_UP, 23, .62f)
                        holdPair()
                        sendContacts(MotionEvent.ACTION_CANCEL, listOf(Triple(7, .45f, .5f), Triple(23, .55f, .5f)))
                        holdPair()
                        sendContacts(MotionEvent.ACTION_POINTER_UP, listOf(Triple(7, .45f, .5f), Triple(23, .55f, .5f)))
                        send(MotionEvent.ACTION_UP, 23, .55f)
                        scenario.onActivity { assertArrayEquals("Android contacts retained", intArrayOf(), requireNotNull(findDisplay(it.window.decorView)).inputObservations().getIntArray("androidContactIds")) }
                        snapshot(label)
                    }
                    try {
                        resume(); scenario.onActivity { it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT }
                        readyDisplay(Configuration.ORIENTATION_PORTRAIT); resume()
                        val baseline = snapshot("portrait-baseline")
                        holdPair()
                        clickControl("Pause"); awaitState("paused")
                        scenario.onActivity {
                            val view = requireNotNull(findDisplay(it.window.decorView))
                            assertArrayEquals("Pause retained Android contacts", intArrayOf(), view.inputObservations().getIntArray("androidContactIds"))
                            assertTrue("Pause allowed continuation without a fresh DOWN", view.inputObservations().getBoolean("awaitingNewDown"))
                        }
                        snapshot("explicit-pause")
                        clickControl("Resume"); awaitState("running"); readyDisplay(); Thread.sleep(300)
                        reuseAndRelease("explicit-pause-return")
                        holdPair()
                        val oldDisplay = readyDisplay()
                        var beforeCancel = 0L
                        instrumentation.runOnMainSync { beforeCancel = oldDisplay.inputObservations().getLong("cancellationRequests") }
                        scenario.recreate(); readyDisplay(Configuration.ORIENTATION_PORTRAIT)
                        awaitState("paused")
                        instrumentation.runOnMainSync {
                            assertTrue("Recreation did not cancel old view contacts", oldDisplay.inputObservations().getLong("cancellationRequests") > beforeCancel)
                            assertArrayEquals(intArrayOf(), oldDisplay.inputObservations().getIntArray("androidContactIds"))
                        }
                        resume(); reuseAndRelease("recreated-portrait")
                        for ((orientation, configuration, label) in listOf(
                            Triple(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, Configuration.ORIENTATION_LANDSCAPE, "landscape"),
                            Triple(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT, Configuration.ORIENTATION_PORTRAIT, "portrait-return"))) {
                            holdPair()
                            val prior = readyDisplay(); var cancels = 0L
                            instrumentation.runOnMainSync { cancels = prior.inputObservations().getLong("cancellationRequests") }
                            scenario.onActivity { it.requestedOrientation = orientation }
                            readyDisplay(configuration); awaitState("paused")
                            instrumentation.runOnMainSync { assertTrue("Orientation failed to cancel held contacts", prior.inputObservations().getLong("cancellationRequests") > cancels) }
                            resume(); reuseAndRelease(label)
                        }
                        holdPair()
                        scenario.moveToState(Lifecycle.State.CREATED); awaitState("paused")
                        snapshot("background-surface-loss", false)
                        scenario.moveToState(Lifecycle.State.RESUMED); readyDisplay(); awaitState("paused")
                        resume(); reuseAndRelease("foreground-surface-return")
                        val beforeButtons = snapshot("before-buttons")
                        clickControl("Home"); Thread.sleep(300)
                        val sleepBaseline = snapshot("before-power-sleep", false)
                        assertEquals("Guest must be awake before the isolated sleep press", 0L, sleepBaseline.getLong("displaySleeping"))
                        assertTrue("Guest backlight must be on before sleep", sleepBaseline.getLong("backlightLevel") > 0)
                        val asleep = powerTransition("power-sleep", sleepBaseline.getLong("producedFrames"), false)
                        powerTransition("power-wake", asleep.getLong("producedFrames"), true)
                        val afterButtons = snapshot("balanced-buttons")
                        val downBefore = requireNotNull(beforeButtons.getLongArray("buttonDownAdmissions")); val upBefore = requireNotNull(beforeButtons.getLongArray("buttonUpAdmissions"))
                        val downAfter = requireNotNull(afterButtons.getLongArray("buttonDownAdmissions")); val upAfter = requireNotNull(afterButtons.getLongArray("buttonUpAdmissions"))
                        for (button in 0..1) {
                            assertTrue("Button $button never admitted", downAfter[button] > downBefore[button])
                            assertEquals("Unbalanced button $button", downAfter[button] - downBefore[button], upAfter[button] - upBefore[button])
                        }
                        assertEquals(beforeButtons.getLong("buttonRejections"), afterButtons.getLong("buttonRejections"))
                        assertTrue("Production Surface did not reconnect", afterButtons.getLong("surfaceGeneration") > baseline.getLong("surfaceGeneration"))
                        assertTrue("No real frame presented", afterButtons.getLong("presentedFrames") > 0)
                        assertEquals("Window errors increased", baseline.getLong("windowFailures"), afterButtons.getLong("windowFailures"))
                        assertEquals("Frame copy errors increased", baseline.getLong("frameCopyFailures"), afterButtons.getLong("frameCopyFailures"))
                        result.put("hostChecksPassed", true).put("mappingContract", "upright guest portrait pixels in aspect-fit Surface; native maps view-local input without a second rotation")
                    } finally {
                        // Best effort releases never turn a failed test into a pass.
                        try {
                            worker.cancelInput(); worker.button(0, false); worker.button(1, false)
                            wire?.drain(events.length() - 1)
                            result.put("cleanupWorkerStatus", encode(worker.status()))
                        }
                        catch (failure: Exception) { result.put("releaseFailure", failure.toString()) }
                        try { scenario.onActivity { it.requestedOrientation = originalOrientation }; result.put("orientationRestoreRequested", true) }
                        catch (failure: Exception) { result.put("orientationRestoreFailure", failure.toString()) }
                    }
                }
            }
        } catch (failure: Throwable) { result.put("failure", failure.toString()); throw failure }
        finally { persist() }
    }
}
