package org.lighttouch.android

import android.content.Context
import android.content.Intent
import android.content.ComponentName
import android.content.ServiceConnection
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.os.Process
import android.os.IBinder
import android.os.SystemClock
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

/** Explicit creation/arming or read-only observation; never starts/resumes a guest. */
@RunWith(AndroidJUnit4::class)
class GuestSessionSetupTest {
    private fun withExistingWorker(context: Context, action: (IEngine) -> Unit) {
        val connected = CountDownLatch(1)
        var worker: IEngine? = null
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                worker = IEngine.Stub.asInterface(binder); connected.countDown()
            }
            override fun onServiceDisconnected(name: ComponentName) = Unit
        }
        // Flags0 deliberately omit BIND_AUTO_CREATE: no new worker if it died.
        check(context.bindService(Intent(context, EngineService::class.java), connection, 0)) { "Existing worker could not be bound" }
        try {
            check(connected.await(10, TimeUnit.SECONDS)) { "No existing guest worker connection" }
            action(requireNotNull(worker))
        } finally { context.unbindService(connection) }
    }
    @Test fun createUnusedForegroundServiceForLaterArming() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit unused service creation required", arguments.getString("createUnusedGuestWorker") == "true")
        val expectedApk = requireNotNull(arguments.getString("expectedMainApkSha256")) { "Expected main APK hash required" }
        require(expectedApk.matches(Regex("[0-9a-f]{64}"))) { "Invalid expected APK hash" }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "session-create-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val receipt = JSONObject().put("pass", false).put("guestAcceptance", false)
            .put("createsServiceMaySpawnWorker", true).put("serviceStartRequested", false)
            .put("guestStartedByTest", false).put("workerRestartedByTest", false)
            .put("priorWorkerAbsenceProven", false).put("traceArmedByTest", false)
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is LongArray -> put(key, JSONArray(value.toList()))
                is IntArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value)
            }
        }
        fun unused(status: Bundle) {
            assertEquals(Process.myUid(), status.getInt("uid")); assertNotEquals(Process.myPid(), status.getInt("pid"))
            assertEquals("stopped", status.getString("state")); assertFalse(status.getBoolean("sessionStarted"))
            assertFalse(status.getBoolean("nativePollSnapshotValid")); assertEquals("", status.getString("error"))
            assertEquals(0L, status.getBundle("observations")!!.getLong("producedFrames"))
        }
        try {
            assertTrue("Debug build required", context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0)
            val actualApk = acceptanceSha256(File(context.applicationInfo.sourceDir))
            receipt.put("mainApkSha256", actualApk).put("expectedMainApkSha256", expectedApk)
            assertEquals("Installed main APK identity", expectedApk, actualApk)
            receipt.put("serviceStartRequested", true).put("serviceStartHostElapsedNs", SystemClock.elapsedRealtimeNanos())
            val component = requireNotNull(context.startForegroundService(Intent(context, EngineService::class.java)))
            assertEquals(ComponentName(context, EngineService::class.java), component)
            receipt.put("serviceComponent", component.flattenToString())
            withExistingWorker(context) { worker ->
                val initial = worker.status()
                receipt.put("before", encode(initial)).put("beforeHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                unused(initial)
                val after = worker.status()
                receipt.put("after", encode(after)).put("afterHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                unused(after); assertEquals(initial.getInt("pid"), after.getInt("pid")); assertEquals(initial.getInt("uid"), after.getInt("uid"))
                receipt.put("pass", true).put("workerPid", after.getInt("pid")).put("appUid", after.getInt("uid"))
            }
        } catch (failure: Throwable) { receipt.put("failure", failure.toString()); throw failure }
        finally { File(evidence, "result.json").writeText(receipt.toString(2)) }
    }
    @Test fun observeExistingUnusedWorkerWithoutArming() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit unused-worker observation required", arguments.getString("observeGuestSession") == "true")
        val expectedApk = requireNotNull(arguments.getString("expectedMainApkSha256")).also {
            require(it.matches(Regex("[0-9a-f]{64}"))) { "Exact main APK SHA256 required" }
        }
        val expectedPid = requireNotNull(arguments.getString("expectedWorkerPid")?.toIntOrNull()).also { require(it > 0) }
        val expectedTicks = requireNotNull(arguments.getString("expectedWorkerStartTicks")).also {
            require((it.toLongOrNull() ?: 0) > 0) { "Exact positive worker start ticks required" }
        }
        val expectedBoot = requireNotNull(arguments.getString("expectedBootId")).also {
            require(it.matches(Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")))
        }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "unused-observation-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val receipt = JSONObject().put("pass", false).put("guestAcceptance", false)
            .put("guestStartedByTest", false).put("workerRestartedByTest", false)
            .put("inputArmingCalls", 0).put("ordinaryForegroundCalls", 0).put("bindingFlags", 0)
            .put("expectedMainApkSha256", expectedApk).put("expectedWorkerPid", expectedPid)
            .put("expectedWorkerStartTicks", expectedTicks).put("expectedBootId", expectedBoot)
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is LongArray -> put(key, JSONArray(value.toList()))
                is IntArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value ?: JSONObject.NULL)
            }
        }
        fun processIdentity() {
            assertNotEquals("Worker must remain a separate process", Process.myPid(), expectedPid)
            assertEquals("Android boot changed", expectedBoot, File("/proc/sys/kernel/random/boot_id").readText().trim())
            val fields = File("/proc/$expectedPid/stat").readText().substringAfterLast(')').trim().split(Regex("\\s+"))
            assertEquals("Worker PID reused", expectedTicks, fields[19])
            assertFalse("Worker retired", fields[0] in listOf("Z", "X"))
            val uid = File("/proc/$expectedPid/status").useLines { rows -> rows.first { it.startsWith("Uid:") } }
                .substringAfter(':').trim().split(Regex("\\s+"))[0].toInt()
            assertEquals("Worker UID changed", Process.myUid(), uid)
        }
        fun unused(state: Bundle) {
            val required = setOf("pid", "uid", "state", "sessionStarted", "nativePollSnapshotValid", "error",
                "nativeStateDirty", "nativeStorageFailed", "graphicsProviderChoice", "graphicsProviderFixed",
                "touchTraceConfigured", "observations")
            assertTrue("Incomplete unused-worker status", state.keySet().containsAll(required))
            assertEquals(expectedPid, state.getInt("pid")); assertEquals(Process.myUid(), state.getInt("uid"))
            assertEquals("stopped", state.getString("state")); assertFalse(state.getBoolean("sessionStarted"))
            assertFalse(state.getBoolean("nativePollSnapshotValid")); assertEquals("", state.getString("error"))
            assertFalse(state.getBoolean("nativeStateDirty")); assertFalse(state.getBoolean("nativeStorageFailed"))
            assertEquals("angle-gl", state.getString("graphicsProviderChoice")); assertTrue(state.getBoolean("graphicsProviderFixed"))
            assertFalse("Pre-native trace configuration is not delivery qualification", state.getBoolean("touchTraceConfigured"))
            val observations = requireNotNull(state.getBundle("observations"))
            assertTrue("Missing producer-frame status", observations.containsKey("producedFrames"))
            assertEquals(0L, observations.getLong("producedFrames"))
        }
        try {
            assertTrue("Debug build required", context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0)
            val actualApk = acceptanceSha256(File(context.applicationInfo.sourceDir))
            receipt.put("mainApkSha256", actualApk)
            assertEquals("Installed main APK identity", expectedApk, actualApk)
            processIdentity()
            withExistingWorker(context) { worker ->
                processIdentity()
                val before = worker.status()
                receipt.put("before", encode(before)).put("beforeHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                processIdentity(); unused(before)
                val after = worker.status()
                receipt.put("after", encode(after)).put("afterHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                processIdentity(); unused(after)
                assertEquals("Installed main APK changed during observation", expectedApk,
                    acceptanceSha256(File(context.applicationInfo.sourceDir)))
                processIdentity()
                receipt.put("workerPid", expectedPid).put("appUid", Process.myUid()).put("pass", true)
            }
        } catch (failure: Throwable) { receipt.put("failure", failure.toString()); throw failure }
        finally { File(evidence, "result.json").writeText(receipt.toString(2)) }
    }
    @Test fun armExistingUnusedWorkerBeforeOperatorStart() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit session setup opt-in required", arguments.getString("armGuestSession") == "true")
        require(arguments.getString("armTouchWireTrace") == "true") { "Explicit touch wire arming required" }
        val expectedApk = requireNotNull(arguments.getString("expectedMainApkSha256")) { "Expected main APK hash required" }
        require(expectedApk.matches(Regex("[0-9a-f]{64}"))) { "Invalid expected APK hash" }
        val expectedPid = arguments.getString("expectedWorkerPid")?.toIntOrNull()
        require(expectedPid != null && expectedPid > 0) { "Existing worker PID required" }
        val graphicsProvider = arguments.getString("armGraphicsProvider")
        require(graphicsProvider == null || graphicsProvider == "angle-gl") { "armGraphicsProvider must be angle-gl when supplied" }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "session-setup-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val receipt = JSONObject().put("pass", false).put("guestAcceptance", false)
            .put("guestStartedByTest", false).put("workerRestartedByTest", false)
            .put("traceDeliveryVerified", false).put("qmpEndpointVerified", false)
            .put("graphicsProviderRequested", graphicsProvider ?: JSONObject.NULL)
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is LongArray -> put(key, JSONArray(value.toList()))
                is IntArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value)
            }
        }
        try {
            assertTrue("Debug build required", context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0)
            val actualApk = acceptanceSha256(File(context.applicationInfo.sourceDir))
            receipt.put("mainApkSha256", actualApk).put("expectedMainApkSha256", expectedApk)
            assertEquals("Installed main APK identity", expectedApk, actualApk)
            withExistingWorker(context) { worker ->
                val initial = worker.status()
                receipt.put("before", encode(initial)).put("beforeHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                assertEquals(Process.myUid(), initial.getInt("uid"))
                assertNotEquals(Process.myPid(), initial.getInt("pid"))
                assertEquals("No replacement worker may be armed", expectedPid, initial.getInt("pid"))
                assertEquals("Existing unused worker required", "stopped", initial.getString("state"))
                assertFalse("Consumed workers cannot be armed", initial.getBoolean("sessionStarted"))
                assertFalse("No native session may exist", initial.getBoolean("nativePollSnapshotValid"))
                assertEquals("Retain actual faults; do not acknowledge them here", "", initial.getString("error"))
                assertEquals(0L, initial.getBundle("observations")!!.getLong("producedFrames"))
                if (graphicsProvider != null) {
                    val selectionError = worker.debugSelectGraphicsProvider(1)
                    val selected = worker.status()
                    receipt.put("graphicsProviderSelectionError", selectionError).put("graphicsProviderAfterSelection", encode(selected))
                    assertEquals("Fixed pre-start graphics provider admission", "", selectionError)
                    assertEquals(initial.getInt("pid"), selected.getInt("pid")); assertEquals(initial.getInt("uid"), selected.getInt("uid"))
                    assertEquals("stopped", selected.getString("state")); assertFalse(selected.getBoolean("sessionStarted"))
                    assertFalse(selected.getBoolean("nativePollSnapshotValid")); assertEquals("", selected.getString("error"))
                    assertEquals(0L, selected.getBundle("observations")!!.getLong("producedFrames"))
                    assertEquals(graphicsProvider, selected.getString("graphicsProviderChoice")); assertTrue(selected.getBoolean("graphicsProviderFixed"))
                }
                val traceError = worker.debugEnableTouchTrace()
                receipt.put("touchTraceArmError", traceError).put("touchTraceArmAdmissionAccepted", traceError.isEmpty())
                assertEquals("Fixed pre-start touch trace admission", "", traceError)
                val armQmp = arguments.getString("armFixedQmp") == "true"
                receipt.put("fixedQmpArmRequested", armQmp)
                if (armQmp) {
                    val qmpError = worker.debugArmQmp()
                    receipt.put("fixedQmpArmError", qmpError).put("fixedQmpArmAdmissionAccepted", qmpError.isEmpty())
                    assertEquals("Fixed pre-start QMP admission", "", qmpError)
                }
                val after = worker.status()
                receipt.put("after", encode(after)).put("afterHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                    .put("touchTraceConfiguredAfterArm", after.getBoolean("touchTraceConfigured"))
                    .put("graphicsProviderObserved", after.getString("graphicsProviderChoice"))
                    .put("graphicsProviderFixed", after.getBoolean("graphicsProviderFixed"))
                if (graphicsProvider != null) {
                    assertEquals(graphicsProvider, after.getString("graphicsProviderChoice")); assertTrue(after.getBoolean("graphicsProviderFixed"))
                }
                assertEquals(initial.getInt("pid"), after.getInt("pid")); assertEquals(initial.getInt("uid"), after.getInt("uid"))
                assertEquals("stopped", after.getString("state")); assertFalse(after.getBoolean("sessionStarted"))
                assertFalse(after.getBoolean("nativePollSnapshotValid")); assertEquals("", after.getString("error"))
                assertEquals(0L, after.getBundle("observations")!!.getLong("producedFrames"))
                receipt.put("pass", true).put("workerPid", after.getInt("pid")).put("appUid", after.getInt("uid"))
            }
        } catch (failure: Throwable) { receipt.put("failure", failure.toString()); throw failure }
        finally { File(evidence, "result.json").writeText(receipt.toString(2)) }
    }
    /** Capture one current native framebuffer without a UI notice or Activity recreation. */
    @Test fun captureExistingRunningKeyboardFrameWithoutUiChanges() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit current-frame capture required", arguments.getString("captureReviewedKeyboardFrame") == "true")
        val expectedApk = requireNotNull(arguments.getString("expectedMainApkSha256")).also {
            require(it.matches(Regex("[0-9a-f]{64}")))
        }
        val expectedPid = requireNotNull(arguments.getString("expectedWorkerPid")?.toIntOrNull()).also { require(it > 0) }
        val expectedTicks = requireNotNull(arguments.getString("expectedWorkerStartTicks")).also {
            require((it.toLongOrNull() ?: 0) > 0)
        }
        val expectedBoot = requireNotNull(arguments.getString("expectedBootId")).also {
            require(it.matches(Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")))
        }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "frame-observation-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val receipt = JSONObject().put("pass", false).put("guestAcceptance", false)
            .put("guestStartedByTest", false).put("workerRestartedByTest", false)
            .put("inputArmingCalls", 0).put("ordinaryForegroundCalls", 0).put("bindingFlags", 0)
            .put("activityRecreationCalls", 0).put("atomicFrameBindingClaimed", false)
            .put("expectedMainApkSha256", expectedApk).put("expectedWorkerPid", expectedPid)
            .put("expectedWorkerStartTicks", expectedTicks).put("expectedBootId", expectedBoot)
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is LongArray -> put(key, JSONArray(value.toList()))
                is IntArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value ?: JSONObject.NULL)
            }
        }
        fun identity() {
            assertNotEquals(Process.myPid(), expectedPid)
            assertEquals(expectedBoot, File("/proc/sys/kernel/random/boot_id").readText().trim())
            val fields = File("/proc/$expectedPid/stat").readText().substringAfterLast(')').trim().split(Regex("\\s+"))
            assertEquals(expectedTicks, fields[19]); assertFalse(fields[0] in listOf("Z", "X"))
            val uid = File("/proc/$expectedPid/status").useLines { rows -> rows.first { it.startsWith("Uid:") } }
                .substringAfter(':').trim().split(Regex("\\s+"))[0].toInt()
            assertEquals(Process.myUid(), uid)
        }
        fun running(status: Bundle) {
            assertTrue(status.keySet().containsAll(setOf("pid", "uid", "state", "sessionStarted", "nativePollSnapshotValid", "error", "nativeStorageFailed", "observations")))
            assertEquals(expectedPid, status.getInt("pid")); assertEquals(Process.myUid(), status.getInt("uid"))
            assertEquals("running", status.getString("state")); assertTrue(status.getBoolean("sessionStarted"))
            assertTrue(status.getBoolean("nativePollSnapshotValid")); assertEquals("", status.getString("error"))
            assertFalse(status.getBoolean("nativeStorageFailed"))
            val observation = requireNotNull(status.getBundle("observations"))
            assertTrue(observation.keySet().containsAll(setOf("engineReady", "producedFrames", "displaySleeping", "backlightLevel", "guestFrameWidth", "guestFrameHeight")))
            assertEquals(1L, observation.getLong("engineReady")); assertTrue(observation.getLong("producedFrames") > 0)
            assertEquals(0L, observation.getLong("displaySleeping")); assertTrue(observation.getLong("backlightLevel") > 0)
            assertTrue(observation.getLong("guestFrameWidth") in 1L..4096L)
            assertTrue(observation.getLong("guestFrameHeight") in 1L..4096L)
        }
        try {
            assertTrue(context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0)
            assertEquals(expectedApk, acceptanceSha256(File(context.applicationInfo.sourceDir))); identity()
            withExistingWorker(context) { worker ->
                identity(); val before = worker.status(); running(before)
                receipt.put("before", encode(before)).put("beforeHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                val frame = File(context.filesDir, "screenshots/${evidence.name}/keyboard.png")
                receipt.put("screenshot", frame.absolutePath).put("captureBeginHostNs", SystemClock.elapsedRealtimeNanos())
                val error = worker.screenshot(frame.absolutePath)
                receipt.put("captureEndHostNs", SystemClock.elapsedRealtimeNanos()).put("captureError", error)
                assertEquals("One actual native screenshot", "", error)
                identity(); val after = worker.status(); running(after)
                receipt.put("after", encode(after)).put("afterHostElapsedNs", SystemClock.elapsedRealtimeNanos())
                assertTrue("Bounded original PNG required", frame.length() in 1L..8_388_608L)
                val options = android.graphics.BitmapFactory.Options().apply { inJustDecodeBounds = true }
                android.graphics.BitmapFactory.decodeFile(frame.absolutePath, options)
                assertEquals("image/png", options.outMimeType)
                val observations = requireNotNull(after.getBundle("observations"))
                assertEquals(observations.getLong("guestFrameWidth").toInt(), options.outWidth)
                assertEquals(observations.getLong("guestFrameHeight").toInt(), options.outHeight)
                receipt.put("imageSha256", acceptanceSha256(frame)).put("frameWidth", options.outWidth).put("frameHeight", options.outHeight)
                assertEquals(expectedApk, acceptanceSha256(File(context.applicationInfo.sourceDir))); identity()
                receipt.put("pass", true).put("workerPid", expectedPid).put("appUid", Process.myUid())
            }
        } catch (failure: Throwable) { receipt.put("failure", failure.toString()); throw failure }
        finally { File(evidence, "result.json").writeText(receipt.toString(2)) }
    }
}
