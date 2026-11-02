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

/** Uses only attempt10's existing interface: resume, status, fixed FRONTMOST, captures. */
@RunWith(AndroidJUnit4::class)
class GuestFrontmostDiagnosisTest {
    @Test fun resumeAndReadActualFrontmostWithBoundedAgentWait() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit read-only guest diagnosis required", arguments.getString("diagnoseGuestFrontmost") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "frontmost-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("diagnosisCaptured", false).put("guestAcceptancePassed", false)
            .put("visibleGuestVerified", false).put("audiblePlaybackVerified", false).put("operation", "FRONTMOST")
        val timeline = JSONArray()
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is LongArray -> put(key, JSONArray(value.toList()))
                is IntArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value)
            }
        }
        fun persist() {
            File(evidence, "result.json").writeText(result.toString(2))
            File(evidence, "timeline.json").writeText(timeline.toString(2))
        }
        try {
            ActivityScenario.launch(MainActivity::class.java).use {
                AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                    val worker = binding.get()
                    val initial = worker.status()
                    result.put("initialWorker", encode(initial)); persist()
                    assertTrue("Existing session required; this test never starts a guest", initial.getBoolean("sessionStarted"))
                    assertTrue(initial.toString(), initial.getString("state") in listOf("running", "paused"))
                    val pid = initial.getInt("pid")
                    assertEquals(Process.myUid(), initial.getInt("uid")); assertNotEquals(Process.myPid(), pid)
                    result.put("workerPid", pid).put("appUid", Process.myUid())
                    worker.pause(false)
                    val started = SystemClock.elapsedRealtime()
                    val deadline = started + 15_000
                    var alive = false
                    do {
                        val status = worker.status()
                        assertEquals("Worker changed", pid, status.getInt("pid"))
                        timeline.put(JSONObject().put("label", "await-agent").put("hostElapsedMs", SystemClock.elapsedRealtime()).put("status", encode(status))); persist()
                        check(status.getString("state") !in listOf("failed", "stopped")) { status.toString() }
                        val observed = status.getBundle("observations")
                        alive = observed?.getBoolean("guestAgentStateAvailable") == true && observed.getLong("guestAgentState") == 1L
                        if (alive) break
                        Thread.sleep(minOf(200, (deadline - SystemClock.elapsedRealtime()).coerceAtLeast(1)))
                    } while (SystemClock.elapsedRealtime() < deadline)
                    result.put("agentWaitMs", SystemClock.elapsedRealtime() - started).put("agentAliveObserved", alive); persist()
                    assertTrue("Agent did not become alive within 15 seconds; retained status distinguishes absent/stale", alive)
                    fun capture(label: String) {
                        val status = worker.status()
                        val target = File(context.filesDir, "screenshots/${evidence.name}/$label.png")
                        val item = JSONObject().put("label", label).put("hostElapsedMs", SystemClock.elapsedRealtime()).put("status", encode(status))
                        timeline.put(item); persist()
                        val error = worker.screenshot(target.absolutePath)
                        item.put("capture", target.absolutePath).put("captureError", error); persist()
                        assertEquals("Actual frame capture", "", error)
                    }
                    capture("before-frontmost")
                    val queued = worker.guestRequest(1, "", 10_000)
                    result.put("request", encode(queued)); persist()
                    assertEquals("Fixed FRONTMOST admission", "", queued.getString("error"))
                    val id = queued.getLong("requestId")
                    assertTrue("Missing owned request ID", id > 0)
                    try {
                        val replyDeadline = SystemClock.elapsedRealtime() + 11_000
                        var terminal: Bundle? = null
                        do {
                            val reply = worker.guestPoll(id)
                            result.put("response", encode(reply)); persist()
                            assertEquals("Fixed FRONTMOST poll", "", reply.getString("error"))
                            if (reply.getInt("state") in listOf(2, 3, 4, 5)) { terminal = reply; break }
                            Thread.sleep(100)
                        } while (SystemClock.elapsedRealtime() < replyDeadline)
                        val reply = requireNotNull(terminal) { "FRONTMOST did not finish in its bounded interval" }
                        capture("after-frontmost")
                        assertEquals(reply.toString(), 2, reply.getInt("state"))
                        assertEquals(reply.toString(), 0, reply.getInt("guestStatus"))
                        // Retain the entire native-owned UTF8 output, including title.
                        result.put("frontmostOutput", reply.getString("output")).put("terminalWorker", encode(worker.status()))
                            .put("diagnosisCaptured", true)
                    } finally { worker.guestCancel(id) }
                }
            }
        } catch (failure: Throwable) { result.put("failure", failure.toString()); throw failure }
        finally { persist() }
    }
}
