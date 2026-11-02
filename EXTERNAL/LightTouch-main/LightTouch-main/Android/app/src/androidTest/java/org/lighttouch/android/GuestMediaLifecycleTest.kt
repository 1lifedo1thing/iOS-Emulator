package org.lighttouch.android

import android.content.Intent
import android.graphics.BitmapFactory
import android.media.*
import android.os.*
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
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
import java.security.MessageDigest

/** Observe real production playback/presentation around lifecycle transitions. */
@RunWith(AndroidJUnit4::class)
class GuestMediaLifecycleTest {
    @Test fun productionMediaContinuesAfterPauseSurfaceAndAudioFocusChanges() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Requires an actual running guest and explicit media opt-in", arguments.getString("exerciseMedia") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val heldFocus = arguments.getString("exerciseHeldAudioFocus") == "true"
        val requireConcurrentWindows = arguments.getString("requireConcurrentMediaWindows") == "true"
        // A single prereviewed JSON binding; absent means the old passive test.
        val keyboard = arguments.getString("keyboardWorkloadBinding")?.let { JSONObject(it) }
        // Diagnostic successor: retain each typed/erased state before the next
        // action. This never forces a frame or changes the overlap predicate.
        val visibleKeyboardPhases = arguments.getString("keyboardVisiblePhases") == "true"
        require(arguments.getString("keyboardVisiblePhases") in listOf(null, "true"))
        if (visibleKeyboardPhases) {
            require(keyboard != null && requireConcurrentWindows)
            require(arguments.getString("mediaSeconds", "30") == "30")
        }
        val captureFrameTimeline = arguments.getString("captureFrameTimeline") == "true"
        require(arguments.getString("captureFrameTimeline") in listOf(null, "true"))
        if (captureFrameTimeline) require(visibleKeyboardPhases)
        keyboard?.let { plan ->
            require(plan.getInt("schema") == 1 && plan.getBoolean("scratchNoteApproved") && plan.getBoolean("preservesExistingTestNote"))
            require(plan.getString("scratchNoteIdentity").length in 1..120)
            require(plan.getString("reviewedGuestCaptureSha256").matches(Regex("[0-9a-f]{64}")))
            require(plan.getInt("workerPid") > 0 && plan.getInt("workerUid") > 0 && plan.getLong("workerStartTicks") > 0)
            require(arguments.getString("mediaSeconds", "30") in listOf("30", "1200"))
            for (name in listOf("view", "frame")) {
                val size = plan.getJSONArray(name)
                require(size.length() == 2 && (0..1).all { size.getInt(it) in 1..4096 })
            }
            require(plan.getInt("rotation") in 0..3)
            for (name in listOf("letter", "backspace")) {
                val point = plan.getJSONArray(name)
                require(point.length() == 2 && (0..1).all { point.getDouble(it).isFinite() && point.getDouble(it) > 0 && point.getDouble(it) < 1 })
            }
            require(plan.getJSONArray("letter").toString() != plan.getJSONArray("backspace").toString())
            val anchors = plan.getJSONArray("anchors")
            require(anchors.length() in 2..8)
            var pixels = 0L
            for (i in 0 until anchors.length()) {
                val anchor = anchors.getJSONObject(i); val box = anchor.getJSONArray("bounds")
                require(box.length() == 4 && box.getInt(0) >= 0 && box.getInt(1) >= 0 &&
                    box.getInt(2) > box.getInt(0) && box.getInt(3) > box.getInt(1) &&
                    box.getInt(2) <= plan.getJSONArray("frame").getInt(0) && box.getInt(3) <= plan.getJSONArray("frame").getInt(1))
                pixels += (box.getInt(2) - box.getInt(0)).toLong() * (box.getInt(3) - box.getInt(1))
                require(anchor.getString("rgbaSha256").matches(Regex("[0-9a-f]{64}")))
            }
            require(pixels <= 65_536)
        }
        val extended = heldFocus || requireConcurrentWindows || keyboard != null
        if (heldFocus) assertEquals("Held focus opt-in also requires real focus exercise", "true", arguments.getString("exerciseAudioFocus"))
        val evidence = File(context.filesDir, "media-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("pass", false).put("audiblePlaybackVerified", false).put("visibleGuestVerified", false)
            .put("guestAcceptancePassed", false).put("heldAudioFocusRequested", heldFocus)
            .put("concurrentWindowsRequired", requireConcurrentWindows)
            .put("visibleKeyboardPhases", visibleKeyboardPhases)
            .put("frameTimelineRequested", captureFrameTimeline)
            .put("visibleKeyboardPhaseMs", if (visibleKeyboardPhases) 1_500 else JSONObject.NULL)
            .put("keyboardWorkloadBinding", keyboard ?: JSONObject.NULL)
        val samples = JSONArray()
        val intervals = JSONArray(); val events = JSONArray()
        fun keyboardEvent(item: JSONObject) {
            events.put(item)
            File(evidence, "keyboard-events.jsonl").appendText(item.toString() + "\n")
        }
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is LongArray -> put(key, JSONArray(value.toList()))
                is IntArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value)
            }
        }
        fun findDisplay(view: View): GuestDisplay? {
            if (view is GuestDisplay) return view
            if (view is ViewGroup) for (i in 0 until view.childCount) findDisplay(view.getChildAt(i))?.let { return it }
            return null
        }
        val manager = context.getSystemService(AudioManager::class.java)
        var competingFocus: AudioFocusRequest? = null
        try {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                    val worker = binding.get()
                    val initial = worker.status()
                    val pid = initial.getInt("pid"); val uid = initial.getInt("uid")
                    var frameTimeline: FrameTimelineProbe? = null
                    var viewAvailable = true
                    fun snapshot(label: String): Bundle {
                        val begin = SystemClock.elapsedRealtimeNanos()
                        val status = worker.status()
                        val item = JSONObject().put("label", label).put("hostElapsedMs", SystemClock.elapsedRealtime()).put("status", encode(status))
                        if (extended) {
                            item.put("readBeginElapsedNs", begin).put("readEndElapsedNs", SystemClock.elapsedRealtimeNanos())
                            if (viewAvailable) scenario.onActivity { activity ->
                                val display = requireNotNull(findDisplay(activity.window.decorView))
                                item.put("androidInput", encode(display.inputObservations())).put("surfaceValid", display.holder.surface.isValid)
                                    .put("viewFocused", display.hasWindowFocus())
                            }
                            else item.put("androidInputUnavailableWhileBackgrounded", true)
                        }
                        samples.put(item)
                        // The extended 1200s trace appends bounded records instead
                        // of rewriting the growing multi-megabyte array every second.
                        if (extended) File(evidence, "observations.jsonl").appendText(item.toString() + "\n")
                        else File(evidence, "observations.json").writeText(samples.toString(2))
                        if (extended) {
                            assertEquals("Worker PID changed", pid, status.getInt("pid")); assertEquals("Worker UID changed", uid, status.getInt("uid"))
                            assertEquals("Current worker failure (historical errors are not cleared)", "", status.getString("error"))
                            if (label == "running") assertEquals("Sustained workload was interrupted", "running", status.getString("state"))
                        }
                        return requireNotNull(status.getBundle("observations"))
                    }
                    fun capture(label: String) {
                        val before = worker.status()
                        val path = File(context.filesDir, "screenshots/${evidence.name}/$label.png")
                        val error = worker.screenshot(path.absolutePath)
                        events.put(JSONObject().put("capture", path.absolutePath).put("label", label).put("error", error)
                            .put("before", encode(before)).put("after", encode(worker.status())).put("atomicFrameBindingClaimed", false))
                        File(evidence, "events.json").writeText(events.toString(2))
                        assertEquals("Actual guest frame capture", "", error)
                        frameTimeline?.let { probe ->
                            val diagnostic = probe.capture(label, worker.status())
                            events.put(JSONObject().put("frameTimeline", diagnostic))
                            File(evidence, "events.json").writeText(events.toString(2))
                            assertTrue("Required read-only frame timeline diagnostic: $diagnostic", diagnostic.getBoolean("pass"))
                        }
                    }
                    var downTime = SystemClock.uptimeMillis()
                    fun touch(action: Int, ids: IntArray) {
                        val status = worker.status()
                        scenario.onActivity { activity ->
                            val display = requireNotNull(findDisplay(activity.window.decorView))
                            assertTrue("Production Surface is unavailable", display.holder.surface.isValid)
                            val viewport = GuestTestViewport(status, display.width, display.height)
                            if ((action and MotionEvent.ACTION_MASK) == MotionEvent.ACTION_DOWN) downTime = SystemClock.uptimeMillis()
                            val points = ids.map { viewport.point(if (it == 7) .45f else .55f, .5f) }
                            val properties = ids.map { MotionEvent.PointerProperties().apply { id = it; toolType = MotionEvent.TOOL_TYPE_FINGER } }.toTypedArray()
                            val coords = points.map { MotionEvent.PointerCoords().apply { x = it.first; y = it.second; pressure = 1f; size = .05f } }.toTypedArray()
                            val at = SystemClock.uptimeMillis()
                            val event = MotionEvent.obtain(downTime, at, action, ids.size, properties, coords, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
                            try { assertTrue(display.dispatchTouchEvent(event)) } finally { event.recycle() }
                            events.put(JSONObject().put("action", action).put("ids", JSONArray(ids.toList())).put("rawUptimeMs", at)
                                .put("viewport", viewport.receipt()).put("androidInput", encode(display.inputObservations())))
                        }
                        File(evidence, "events.json").writeText(events.toString(2))
                    }
                    fun input(): Bundle {
                        var value: Bundle? = null
                        scenario.onActivity { value = requireNotNull(findDisplay(it.window.decorView)).inputObservations() }
                        return requireNotNull(value)
                    }
                    fun awaitState(expected: String) {
                        val deadline = SystemClock.elapsedRealtime() + 10_000
                        while (worker.status().getString("state") != expected && SystemClock.elapsedRealtime() < deadline) Thread.sleep(100)
                        assertEquals("Lifecycle state", expected, worker.status().getString("state"))
                    }
                    var exerciseFailure: Throwable? = null
                    var keyboardDisplay: GuestDisplay? = null
                    var keyboardContactHeld = false
                    try {
                    assertTrue("Root must boot the selected real guest first", initial.getString("state") in listOf("running", "paused"))
                    if (extended) assertEquals("Resolve the existing worker failure before measuring; no acknowledgement shortcut", "", initial.getString("error"))
                    if (keyboard != null) assertEquals("Inspected keyboard must already be running; this workload never resumes an unvalidated worker", "running", initial.getString("state"))
                    if (initial.getString("state") == "paused") worker.pause(false)
                    awaitState("running")
                    val keyboardInitial = if (keyboard != null) worker.status() else null
                    val initialKeyboardOwner = keyboardInitial?.getLong("displayInputOwner") ?: 0L
                    val initialKeyboardGeneration = keyboardInitial?.getLong("displayInputGeneration") ?: 0L
                    val initialKeyboardSurface = keyboardInitial?.getBundle("observations")?.getLong("surfaceGeneration") ?: 0L
                    fun keyboardGuard(status: Bundle) {
                        val plan = requireNotNull(keyboard)
                        assertEquals("Reviewed worker PID", plan.getInt("workerPid"), status.getInt("pid"))
                        assertEquals("Reviewed worker UID", plan.getInt("workerUid"), status.getInt("uid"))
                        val stat = File("/proc/${status.getInt("pid")}/stat").readText()
                        val start = stat.substring(stat.lastIndexOf(')') + 1).trim().split(Regex("\\s+"))[19].toLong()
                        assertEquals("Reviewed worker start token", plan.getLong("workerStartTicks"), start)
                        assertEquals("Keyboard workload requires running worker", "running", status.getString("state"))
                        assertEquals("Keyboard workload worker failure", "", status.getString("error"))
                        assertEquals("Keyboard display owner changed", initialKeyboardOwner, status.getLong("displayInputOwner"))
                        assertEquals("Keyboard input generation changed", initialKeyboardGeneration, status.getLong("displayInputGeneration"))
                        val observed = requireNotNull(status.getBundle("observations"))
                        assertEquals("Keyboard Surface generation changed", initialKeyboardSurface, observed.getLong("surfaceGeneration"))
                        assertEquals("Guest frame width changed", plan.getJSONArray("frame").getLong(0), observed.getLong("guestFrameWidth"))
                        assertEquals("Guest frame height changed", plan.getJSONArray("frame").getLong(1), observed.getLong("guestFrameHeight"))
                    }
                    fun keyboardView(activity: MainActivity, status: Bundle): Pair<GuestDisplay, GuestTestViewport> {
                        keyboardGuard(status)
                        val plan = requireNotNull(keyboard)
                        val display = requireNotNull(findDisplay(activity.window.decorView))
                        if (keyboardDisplay == null) keyboardDisplay = display
                        assertSame("Keyboard View changed", keyboardDisplay, display)
                        assertTrue("Keyboard Surface/focus unavailable", display.holder.surface.isValid && display.hasWindowFocus())
                        assertEquals(plan.getJSONArray("view").getInt(0), display.width)
                        assertEquals(plan.getJSONArray("view").getInt(1), display.height)
                        assertEquals(plan.getInt("rotation"), display.display.rotation)
                        return display to GuestTestViewport(status, display.width, display.height)
                    }
                    fun checkKeyboardImage(label: String) {
                        val plan = requireNotNull(keyboard)
                        keyboardGuard(worker.status()); capture(label)
                        keyboardGuard(worker.status())
                        val path = File(context.filesDir, "screenshots/${evidence.name}/$label.png")
                        val image = requireNotNull(BitmapFactory.decodeFile(path.absolutePath))
                        try {
                            assertEquals(plan.getJSONArray("frame").getInt(0), image.width)
                            assertEquals(plan.getJSONArray("frame").getInt(1), image.height)
                            val anchors = plan.getJSONArray("anchors")
                            for (i in 0 until anchors.length()) {
                                val anchor = anchors.getJSONObject(i); val box = anchor.getJSONArray("bounds")
                                val width = box.getInt(2) - box.getInt(0); val height = box.getInt(3) - box.getInt(1)
                                val pixels = IntArray(width * height)
                                image.getPixels(pixels, 0, width, box.getInt(0), box.getInt(1), width, height)
                                val rgba = ByteArray(pixels.size * 4)
                                pixels.forEachIndexed { n, pixel ->
                                    rgba[n * 4] = (pixel shr 16).toByte(); rgba[n * 4 + 1] = (pixel shr 8).toByte()
                                    rgba[n * 4 + 2] = pixel.toByte(); rgba[n * 4 + 3] = (pixel ushr 24).toByte()
                                }
                                val hash = MessageDigest.getInstance("SHA-256").digest(rgba).joinToString("") { "%02x".format(it.toInt() and 255) }
                                keyboardEvent(JSONObject().put("keyboardAnchor", i).put("captureLabel", label).put("rgbaSha256", hash))
                                assertEquals("Reviewed keyboard anchor changed", anchor.getString("rgbaSha256"), hash)
                            }
                        } finally { image.recycle(); File(evidence, "events.json").writeText(events.toString(2)) }
                    }
                    fun keyboardTap(key: String, pair: Int, cutoff: Long) {
                        val plan = requireNotNull(keyboard); val point = plan.getJSONArray(key)
                        var beforeInput: Bundle? = null
                        var tapDown = 0L
                        for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
                            assertTrue("Keyboard measurement cutoff", SystemClock.elapsedRealtime() < cutoff)
                            val status = worker.status()
                            scenario.onActivity { activity ->
                                val (display, viewport) = keyboardView(activity, status)
                                assertTrue("Keyboard measurement cutoff before dispatch", SystemClock.elapsedRealtime() < cutoff)
                                val before = display.inputObservations()
                                if (action == MotionEvent.ACTION_DOWN) {
                                    assertArrayEquals("External held input", intArrayOf(), before.getIntArray("androidContactIds"))
                                    beforeInput = before; tapDown = SystemClock.uptimeMillis(); keyboardContactHeld = true
                                }
                                val at = SystemClock.uptimeMillis(); val xy = viewport.point(point.getDouble(0).toFloat(), point.getDouble(1).toFloat())
                                val event = MotionEvent.obtain(tapDown, at, action, xy.first, xy.second, 0).apply { source = InputDevice.SOURCE_TOUCHSCREEN }
                                try { assertTrue(display.dispatchTouchEvent(event)) } finally { event.recycle() }
                                val after = display.inputObservations()
                                keyboardEvent(JSONObject().put("keyboardPair", pair).put("key", key).put("action", action).put("rawUptimeMs", at)
                                    .put("elapsedMs", SystemClock.elapsedRealtime()).put("viewport", viewport.receipt())
                                    .put("viewX", xy.first).put("viewY", xy.second).put("beforeInput", encode(before)).put("afterInput", encode(after)))
                                assertEquals(before.getLong("rejectedBatches"), after.getLong("rejectedBatches"))
                                assertEquals(before.getLong("cancellationRequests"), after.getLong("cancellationRequests"))
                                assertEquals(before.getLong("expectedCancellationBatches"), after.getLong("expectedCancellationBatches"))
                                if (action == MotionEvent.ACTION_UP) {
                                    assertArrayEquals(intArrayOf(), after.getIntArray("androidContactIds"))
                                    assertEquals(requireNotNull(beforeInput).getLong("forwardedBatches") + 2, after.getLong("forwardedBatches"))
                                    keyboardContactHeld = false
                                }
                            }
                            if (action == MotionEvent.ACTION_DOWN) Thread.sleep(60)
                        }
                    }
                    if (keyboard != null) {
                        require(initialKeyboardOwner > 0 && initialKeyboardGeneration > 0)
                        keyboardGuard(requireNotNull(keyboardInitial))
                        scenario.onActivity { keyboardView(it, requireNotNull(keyboardInitial)) }
                        if (captureFrameTimeline) frameTimeline = FrameTimelineProbe(context.filesDir, evidence, pid, uid)
                        checkKeyboardImage("keyboard-reviewed-start")
                    }
                    val before = snapshot("measurement-start")
                    if (extended) capture("measurement-start")
                    val durationMs = arguments.getString("mediaSeconds", "30").toLong().coerceIn(5, 1800) * 1000
                    val deadline = SystemClock.elapsedRealtime() + durationMs
                    var nextKeyboardPair = SystemClock.elapsedRealtime()
                    var nextKeyboardImage = nextKeyboardPair + 30_000
                    var keyboardPairs = 0
                    var previous = before; var blockStart = SystemClock.elapsedRealtime(); var qualifyingInBlock = 0; var totalQualifying = 0
                    fun observeRunning(phase: String) {
                        val current = snapshot("running")
                        if (extended) {
                            val interval = JSONObject().put("fromSample", samples.length() - 2).put("toSample", samples.length() - 1)
                                .put("guestConsumptionInferred", false).put("continuousTwoContactsClaimed", false)
                                .put("keyboardPhase", phase)
                            fun delta(key: String, old: Bundle, now: Bundle): Long = (now.getLong(key) - old.getLong(key)).also {
                                assertTrue("Counter reset: $key", it >= 0); interval.put(key + "Delta", it)
                            }
                            val old = requireNotNull(previous.getBundle("audio")); val audio = requireNotNull(current.getBundle("audio"))
                            val overlap = delta("inputRequests", previous, current) > 0 &&
                                delta("producedFrames", previous, current) > 0 && delta("presentedFrames", previous, current) > 0 &&
                                delta("pulledNonzeroSamples", old, audio) > 0 && delta("writtenNonzeroSamples", old, audio) > 0 &&
                                delta("playedFrames", old, audio) > 0
                            // Retain every delta even when an earlier condition is zero.
                            for (key in listOf("inputRequests", "producedFrames", "presentedFrames")) if (!interval.has(key + "Delta")) delta(key, previous, current)
                            for (key in listOf("pulledNonzeroSamples", "writtenNonzeroSamples", "playedFrames", "underruns")) if (!interval.has(key + "Delta")) delta(key, old, audio)
                            interval.put("inputFrameAndNonzeroPlaybackCounterOverlap", overlap)
                            intervals.put(interval); File(evidence, "intervals.jsonl").appendText(interval.toString() + "\n")
                            if (overlap) { qualifyingInBlock++; totalQualifying++ }
                            for (key in listOf("writeFailures", "nativePullFailures")) assertEquals("Audio failure increased: $key", old.getLong(key), audio.getLong(key))
                            for (key in listOf("windowFailures", "frameCopyFailures")) assertEquals("Presentation failure increased: $key", previous.getLong(key), current.getLong(key))
                            if (SystemClock.elapsedRealtime() - blockStart >= 30_000) {
                                if (requireConcurrentWindows) assertTrue("No input/frame/nonzero playback interval in this declared workload block (at most30s)", qualifyingInBlock > 0)
                                blockStart = SystemClock.elapsedRealtime(); qualifyingInBlock = 0
                            }
                        }
                        previous = current
                    }
                    // Reserve both visible waits, captures and the original
                    // three-second no-input tail. No missed pair is caught up.
                    val keyboardPairReserveMs = if (visibleKeyboardPhases) 9_000 else 4_500
                    fun retainVisiblePhase(key: String) {
                        val phaseStart = SystemClock.elapsedRealtime()
                        assertTrue("Visible keyboard phase cannot extend the measurement", phaseStart + 1_500 < deadline - 3_000)
                        Thread.sleep(1_500)
                        assertTrue("Visible keyboard phase crossed the quiet tail", SystemClock.elapsedRealtime() < deadline - 3_000)
                        val label = "keyboard-${keyboardPairs.toString().padStart(4, '0')}-$key-visible"
                        checkKeyboardImage(label)
                        assertTrue("Visible keyboard capture crossed the quiet tail", SystemClock.elapsedRealtime() < deadline - 3_000)
                        keyboardEvent(JSONObject().put("keyboardPair", keyboardPairs).put("visiblePhase", key)
                            .put("phaseStartElapsedMs", phaseStart).put("phaseObservedElapsedMs", SystemClock.elapsedRealtime())
                            .put("expectedTextVisuallyVerified", false).put("captureLabel", label))
                        observeRunning("$key-visible")
                    }
                    while (SystemClock.elapsedRealtime() < deadline) {
                        if (keyboard != null && SystemClock.elapsedRealtime() >= nextKeyboardPair && SystemClock.elapsedRealtime() < deadline - keyboardPairReserveMs) {
                            assertTrue("Keyboard pair bound", keyboardPairs < durationMs / 2_000)
                            if (SystemClock.elapsedRealtime() >= nextKeyboardImage) {
                                checkKeyboardImage("keyboard-${keyboardPairs.toString().padStart(4, '0')}")
                                nextKeyboardImage = SystemClock.elapsedRealtime() + 30_000
                            }
                            val start = SystemClock.elapsedRealtime()
                            assertTrue("Keyboard pair started too near end", start < deadline - keyboardPairReserveMs)
                            keyboardTap("letter", keyboardPairs, deadline - 3_000)
                            if (visibleKeyboardPhases) retainVisiblePhase("letter") else Thread.sleep(250)
                            keyboardTap("backspace", keyboardPairs, deadline - 3_000)
                            if (visibleKeyboardPhases) retainVisiblePhase("erased")
                            keyboardPairs++; nextKeyboardPair = SystemClock.elapsedRealtime() + 2_000
                        }
                        Thread.sleep(minOf(1000, (deadline - SystemClock.elapsedRealtime()).coerceAtLeast(1)))
                        observeRunning("cadence")
                    }
                    if (requireConcurrentWindows && SystemClock.elapsedRealtime() - blockStart >= 5_000)
                        assertTrue("No concurrent counter interval in the final workload block", qualifyingInBlock > 0)
                    result.put("qualifyingCounterOverlapIntervals", totalQualifying)
                    result.put("keyboardPairsCompleted", keyboardPairs).put("keyboardStoppedElapsedMs", SystemClock.elapsedRealtime())
                    if (keyboard != null) {
                        assertTrue("No keyboard pairs completed", keyboardPairs > 0)
                        assertFalse("Keyboard contact held at measurement end", keyboardContactHeld)
                        checkKeyboardImage("keyboard-measurement-end")
                    }
                    val measured = snapshot("measurement-end")
                    if (extended) capture("measurement-end")
                    assertTrue("No actual guest frame reached Surface", measured.getLong("presentedFrames") > 0)
                    val oldAudio = requireNotNull(before.getBundle("audio")); val newAudio = requireNotNull(measured.getBundle("audio"))
                    assertEquals("Audio write failures increased", oldAudio.getLong("writeFailures"), newAudio.getLong("writeFailures"))
                    assertEquals("Native PCM failures increased", oldAudio.getLong("nativePullFailures"), newAudio.getLong("nativePullFailures"))
                    if (arguments.getString("requireFrameProgress") == "true") assertTrue("Chosen animation did not present new frames", measured.getLong("presentedFrames") > before.getLong("presentedFrames"))
                    if (arguments.getString("requireGuestAudio") == "true") {
                        assertTrue("Guest workload supplied no nonzero PCM", newAudio.getLong("pulledNonzeroSamples") > oldAudio.getLong("pulledNonzeroSamples"))
                        assertTrue("AudioTrack accepted no nonzero guest samples", newAudio.getLong("writtenNonzeroSamples") > oldAudio.getLong("writtenNonzeroSamples"))
                        assertTrue("AudioTrack playback head did not advance", newAudio.getLong("playedFrames") > oldAudio.getLong("playedFrames"))
                    }
                    worker.pause(true); awaitState("paused"); Thread.sleep(300)
                    val paused = snapshot("user-paused")
                    Thread.sleep(700)
                    assertEquals("Audio written while user paused", paused.getBundle("audio")!!.getLong("writtenSamples"), snapshot("pause-stable").getBundle("audio")!!.getLong("writtenSamples"))
                    worker.pause(false); awaitState("running"); snapshot("user-resumed")
                    viewAvailable = false
                    scenario.moveToState(Lifecycle.State.CREATED); awaitState("paused"); snapshot("background-surface-loss")
                    scenario.moveToState(Lifecycle.State.RESUMED)
                    viewAvailable = true
                    assertEquals("Reopening must preserve explicit pause", "paused", worker.status().getString("state"))
                    worker.pause(false); awaitState("running"); snapshot("surface-return-resumed")
                    if (arguments.getString("exerciseAudioFocus") == "true") {
                        var heldStatus: Bundle? = null; var heldInput: Bundle? = null
                        if (heldFocus) {
                            touch(MotionEvent.ACTION_DOWN, intArrayOf(7))
                            touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), intArrayOf(7, 23))
                            heldStatus = worker.status(); heldInput = input()
                            assertArrayEquals(intArrayOf(7, 23), heldInput.getIntArray("androidContactIds"))
                            assertTrue("Actual generation is unavailable", heldStatus.getLong("displayInputGeneration") > 0)
                            assertEquals(heldStatus.getLong("displayInputGeneration"), heldInput.getLong("inputGeneration"))
                            capture("focus-two-held")
                        }
                        val focus = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                            .setOnAudioFocusChangeListener({ }, Handler(Looper.getMainLooper())).build()
                        competingFocus = focus
                        assertEquals(AudioManager.AUDIOFOCUS_REQUEST_GRANTED, manager.requestAudioFocus(focus))
                        // Guest audio mixes with other apps' and holds no focus: another app taking it pauses nothing.
                        Thread.sleep(1000); assertEquals("running", snapshot("focus-taken-elsewhere").getString("state"))
                        assertFalse(snapshot("focus-taken-elsewhere").getBoolean("focusPaused"))
                        manager.abandonAudioFocusRequest(focus); competingFocus = null
                        if (heldFocus) {
                            // Nothing cancelled the held contacts either.
                            assertArrayEquals(intArrayOf(7, 23), input().getIntArray("androidContactIds"))
                            touch(MotionEvent.ACTION_POINTER_UP or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), intArrayOf(7, 23))
                            touch(MotionEvent.ACTION_UP, intArrayOf(7))
                            capture("focus-held-kept")
                            result.put("heldFocusHostChecksPassed", true)
                        }
                    }
                    snapshot("terminal")
                    result.put("pass", true).put("workerPid", initial.getInt("pid")).put("appUid", initial.getInt("uid"))
                        .put("measurementMs", durationMs).put("requiresAudio", arguments.getString("requireGuestAudio") == "true")
                        .put("requiresFrameProgress", arguments.getString("requireFrameProgress") == "true")
                    } catch (failure: Throwable) {
                        exerciseFailure = failure
                        // Capture while both the worker binding and ActivityScenario
                        // are still open. The normal end capture follows assertions
                        // and is therefore unreachable on a stalled media workload.
                        // These are observations of the latest published native frame;
                        // they do not prove the guest scanout itself was refreshed.
                        FailureEvidence.capture(failure,
                            { result.put("failureEvidenceAttemptedBeforeActivityClosure", true) },
                            { snapshot("failure-before-native-capture") },
                            { capture("failure-native-frame") },
                            { snapshot("failure-after-native-capture") })
                        throw failure
                    }
                    finally {
                        // Never guess a compensating backspace on a changed scene.
                        // Only retire an in-flight local touch; no workload thread survives.
                        if (heldFocus || keyboardContactHeld) {
                            var cleanup: Throwable? = null
                            fun release(action: () -> Unit) { try { action() } catch (failure: Throwable) { if (cleanup == null) cleanup = failure else cleanup!!.addSuppressed(failure) } }
                            if (keyboardContactHeld) release { scenario.onActivity { keyboardDisplay?.cancelOwnedGesture() } }
                            if (heldFocus) {
                                release { val focus = competingFocus; competingFocus = null; focus?.let { manager.abandonAudioFocusRequest(it) } }
                                if (viewAvailable) release { scenario.onActivity { findDisplay(it.window.decorView)?.cancelOwnedGesture() } }
                                release { worker.cancelInput() }
                            }
                            cleanup?.let { failure ->
                                result.put("pass", false).put("cleanupFailure", failure.toString())
                                if (exerciseFailure == null) throw failure else exerciseFailure!!.addSuppressed(failure)
                            }
                        }
                    }
                }
            }
        } catch (failure: Throwable) { result.put("failure", failure.toString()); throw failure }
        finally {
            competingFocus?.let { manager.abandonAudioFocusRequest(it) }
            if (extended) {
                File(evidence, "observations.json").writeText(samples.toString(2))
                File(evidence, "intervals.json").writeText(intervals.toString(2))
                File(evidence, "events.json").writeText(events.toString(2))
            }
            File(evidence, "result.json").writeText(result.toString(2))
        }
    }
}
