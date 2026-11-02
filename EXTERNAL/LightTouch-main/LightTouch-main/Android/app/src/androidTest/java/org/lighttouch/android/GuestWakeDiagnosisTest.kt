package org.lighttouch.android

import android.content.Intent
import android.os.Bundle
import android.os.Process
import android.os.SystemClock
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

/** Fixed, bounded diagnosis through production controls; no boot/render acceptance. */
@RunWith(AndroidJUnit4::class)
class GuestWakeDiagnosisTest {
    @Test fun samplePowerAgentAndFramesAroundBalancedWakeButtons() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit wake diagnosis required", arguments.getString("diagnoseGuestWake") == "true")
        val holdArgument = arguments.getString("wakePulseHoldMs")
        val pulseHoldMs = if (holdArgument == null) 150 else requireNotNull(holdArgument.toIntOrNull()) {
            "wakePulseHoldMs must be an integer from 150 through 500"
        }
        require(pulseHoldMs in 150..500) { "wakePulseHoldMs must be from 150 through 500" }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "wake-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val receipt = JSONObject().put("diagnosisCaptured", false).put("guestBootProven", false)
            .put("visibleGuestVerified", false).put("audiblePlaybackVerified", false)
            .put("requestedHostPulseHoldMs", pulseHoldMs)
            .put("hostTimestampClock", "Android elapsedRealtime (BOOTTIME)")
            .put("guestVirtualHoldMeasured", false)
        val timeline = JSONArray()
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
            File(evidence, "timeline.json").writeText(timeline.toString(2))
            File(evidence, "result.json").writeText(receipt.toString(2))
        }
        try {
            ActivityScenario.launch(MainActivity::class.java).use {
                AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                    val worker = binding.get()
                    val initial = worker.status()
                    assertEquals(Process.myUid(), initial.getInt("uid")); assertNotEquals(Process.myPid(), initial.getInt("pid"))
                    val workerPid = initial.getInt("pid")
                    receipt.put("workerPid", workerPid).put("appUid", Process.myUid()).put("initialWorker", encode(initial))
                    persist()
                    if (initial.getString("state") == "paused" && arguments.getString("resumeExisting") == "true") worker.pause(false)
                    assertEquals("Existing running guest required; resumeExisting is explicit", "running", worker.status().getString("state"))
                    fun sample(label: String) {
                        val state = worker.status()
                        assertEquals("Worker replaced", workerPid, state.getInt("pid"))
                        val item = JSONObject().put("label", label).put("hostElapsedMs", SystemClock.elapsedRealtime()).put("status", encode(state))
                        timeline.put(item); persist()
                        val capture = File(context.filesDir, "screenshots/${evidence.name}/$label.png")
                        val error = worker.screenshot(capture.absolutePath)
                        item.put("captureError", error).put("capture", capture.absolutePath); persist()
                        assertEquals("Real copied-frame capture", "", error)
                        assertTrue("Runtime diagnostics unavailable", state.getBundle("observations")?.getBoolean("runtimeDiagnosticsAvailable") == true)
                    }
                    fun pulse(button: Int, label: String) {
                        val item = JSONObject().put("button", button).put("label", label).put("downHostElapsedMs", SystemClock.elapsedRealtime())
                        timeline.put(item); persist()
                        try {
                            worker.button(button, true)
                            item.put("downCallReturnedHostElapsedMs", SystemClock.elapsedRealtime()); persist()
                            Thread.sleep(pulseHoldMs.toLong())
                        }
                        finally { worker.button(button, false); item.put("upHostElapsedMs", SystemClock.elapsedRealtime()); persist() }
                        Thread.sleep(2000)
                        sample(label)
                    }
                    try {
                        sample("initial")
                        pulse(0, "after-home")
                        pulse(1, "after-power")
                        pulse(0, "after-return-home")
                        receipt.put("diagnosisCaptured", true).put("buttonAdmissionIsNotDelivery", true)
                    } finally {
                        try { worker.cancelInput(); worker.button(0, false); worker.button(1, false); receipt.put("cleanupWorker", encode(worker.status())) }
                        catch (failure: Exception) { receipt.put("releaseFailure", failure.toString()) }
                    }
                }
            }
        } catch (failure: Throwable) { receipt.put("failure", failure.toString()); throw failure }
        finally { persist() }
    }
}
