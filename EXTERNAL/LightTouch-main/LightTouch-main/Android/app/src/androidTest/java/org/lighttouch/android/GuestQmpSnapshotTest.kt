package org.lighttouch.android

import android.content.Intent
import android.os.*
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
import java.util.zip.ZipFile

/** Debug-only, fixed read-only diagnosis; never passes commands or paths. */
@RunWith(AndroidJUnit4::class)
class GuestQmpSnapshotTest {
    @Test fun readRegistersFromDerivedPrivateEndpointInAppUid() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit debugGuestQmp opt-in required", arguments.getString("debugGuestQmp") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "qmp-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val receipt = JSONObject().put("diagnosisCaptured", false).put("guestBootProven", false)
            .put("hostTimestampClock", "Android SystemClock.elapsedRealtime/BOOTTIME")
            .put("nativeMonotonicClockMustNotBeMixedWithHostTimestampClock", true)
        val snapshots = JSONArray()
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                is IntArray -> put(key, JSONArray(value.toList()))
                is LongArray -> put(key, JSONArray(value.toList()))
                else -> put(key, value)
            }
        }
        fun atomic(name: String, value: String) {
            val pending = File(evidence, ".$name.pending")
            pending.writeText(value)
            check(pending.renameTo(File(evidence, name))) { "Atomic diagnostic publication failed: $name" }
        }
        fun persistSnapshots() = atomic("snapshots.json", snapshots.toString(2))
        fun persistReceipt() = atomic("result.json", receipt.toString(2))
        try {
            receipt.put("installedApkSha256", acceptanceSha256(File(context.applicationInfo.sourceDir)))
            ZipFile(context.applicationInfo.sourceDir).use { apk ->
                val entry = requireNotNull(apk.getEntry("lib/${Build.SUPPORTED_ABIS[0]}/libqemu-arm.so"))
                val digest = MessageDigest.getInstance("SHA-256")
                apk.getInputStream(entry).use { input -> val buffer = ByteArray(65536); while (true) { val size = input.read(buffer); if (size < 0) break; digest.update(buffer, 0, size) } }
                receipt.put("packagedEngineSha256", digest.digest().joinToString("") { "%02x".format(it) })
            }
            receipt.put("firmwareNoticeManifest", JSONObject(context.assets.open("licenses/firmware/manifest.json").bufferedReader().use { it.readText() }))
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                    val worker = binding.get()
                    val initial = worker.status()
                    assertEquals(Process.myUid(), initial.getInt("uid")); assertNotEquals(Process.myPid(), initial.getInt("pid"))
                    val workerPid = initial.getInt("pid")
                    fun checkIdentity(state: Bundle) {
                        assertEquals("Worker PID changed during diagnosis", workerPid, state.getInt("pid"))
                        assertEquals("Worker UID changed during diagnosis", Process.myUid(), state.getInt("uid"))
                    }
                    receipt.put("initialWorker", encode(initial)); persistReceipt()
                    val snapshotOnly = arguments.getString("snapshotOnly") == "true"
                    val touchWireTrace = arguments.getString("touchWireTrace") == "true"
                    val accountingArgument = arguments.getString("debugVcpuAccounting", "false")
                    require(accountingArgument in listOf("true", "false")) { "debugVcpuAccounting must be true or false" }
                    val vcpuAccounting = accountingArgument == "true"
                    val coldRecover = arguments.getString("coldRecover") == "true"
                    val providerArgument = arguments.getString("debugGraphicsProvider")
                    require(providerArgument == null || providerArgument in listOf("platform", "angle-gl")) {
                        "debugGraphicsProvider must be platform or angle-gl"
                    }
                    require(providerArgument == null || !snapshotOnly) {
                        "A provider may only be selected before a fresh worker starts"
                    }
                    require(!coldRecover || !snapshotOnly) { "Cold recovery requires a fresh worker; snapshotOnly cannot recover an existing session" }
                    receipt.put("coldRecoveryRequested", coldRecover).put("shutdownConfirmationInferred", false)
                    receipt.put("touchWireTraceRequested", touchWireTrace).put("initialTouchTraceConfigured", initial.getBoolean("touchTraceConfigured"))
                    receipt.put("vcpuAccountingRequested", vcpuAccounting)
                        .put("initialVcpuAccountingConfigured", initial.getBoolean("vcpuAccountingConfigured"))
                        .put("genuineIosCpuUsageClaimed", false)
                    val record = if (snapshotOnly) arguments.getString("recordId") else arguments.getString("recordId")
                        ?: requireNotNull(worker.records().firstOrNull { it.getBoolean("selected") }?.getString("id"))
                    val started = if (snapshotOnly) {
                        assertTrue("Existing enabled session required; use --no-restart when injecting instrumentation", initial.getBoolean("sessionStarted"))
                        assertFalse("Existing debug endpoint required", initial.getString("debugQmpPath").isNullOrEmpty())
                        if (touchWireTrace) assertTrue("Snapshot-only diagnosis cannot configure trace after start; existing trace required", initial.getBoolean("touchTraceConfigured"))
                        if (vcpuAccounting) assertTrue("Snapshot-only diagnosis cannot arm accounting after start; existing configuration required", initial.getBoolean("vcpuAccountingConfigured"))
                        if (arguments.getString("resumeExisting") == "true") worker.pause(false)
                        worker.status()
                    } else {
                        assertFalse("Root must dispose any consumed worker first", initial.getBoolean("sessionStarted"))
                        assertEquals("stopped", initial.getString("state"))
                        if (providerArgument != null) {
                            assertEquals("Fixed packaged provider selection", "", worker.debugSelectGraphicsProvider(
                                if (providerArgument == "angle-gl") 1 else 0))
                            receipt.put("requestedGraphicsProvider", providerArgument)
                        }
                        if (vcpuAccounting) {
                            assertEquals("Fixed vCPU accounting must be configured before Start", "", worker.debugArmVcpuAccounting())
                            val armed = worker.status()
                            receipt.put("preStartVcpuAccountingConfigured", armed.getBoolean("vcpuAccountingConfigured"))
                            assertTrue("Accounting environment was not configured", armed.getBoolean("vcpuAccountingConfigured"))
                        }
                        if (touchWireTrace) assertEquals("Fixed touch trace must be admitted before QMP/start", "", worker.debugEnableTouchTrace())
                        assertEquals("", worker.debugArmQmp())
                        scenario.onActivity { it.startForegroundService(Intent(it, EngineService::class.java)) }
                        if (coldRecover) worker.recoverStart(requireNotNull(record)) else worker.start(requireNotNull(record))
                    }
                    val requestedHoldSeconds = arguments.getString("qmpHoldSeconds", "0").toLong()
                    checkIdentity(started)
                    receipt.put("startedWorker", encode(started))
                    val holdSeconds = requestedHoldSeconds.coerceIn(0, 3600)
                    receipt.put("snapshotOnly", snapshotOnly).put("requestedHoldSeconds", requestedHoldSeconds)
                        .put("holdSeconds", holdSeconds)
                    assertEquals(started.toString(), "", started.getString("error"))
                    receipt.put("touchTraceConfigured", started.getBoolean("touchTraceConfigured"))
                    if (touchWireTrace) assertTrue("Trace configuration missing from started worker", started.getBoolean("touchTraceConfigured"))
                    receipt.put("vcpuAccountingConfigured", started.getBoolean("vcpuAccountingConfigured"))
                    if (vcpuAccounting) {
                        assertTrue("Accounting configuration missing from started worker", started.getBoolean("vcpuAccountingConfigured"))
                        val refused = worker.debugArmVcpuAccounting()
                        receipt.put("postStartAccountingArmRefusal", refused)
                        assertTrue("A consumed worker must refuse accounting reconfiguration", refused.isNotEmpty())
                    }
                    val endpoint = File(requireNotNull(started.getString("debugQmpPath")))
                    val deadline = SystemClock.elapsedRealtime() + 120_000
                    while (!endpoint.exists() && SystemClock.elapsedRealtime() < deadline) {
                        val state = worker.status(); check(state.getString("state") != "failed") { state.toString() }; Thread.sleep(200)
                    }
                    assertTrue("Private QMP endpoint unavailable; inspect engineLog", endpoint.exists())
                    Thread.sleep(arguments.getString("qmpSettleSeconds", "30").toLong().coerceIn(0, 180) * 1000)
                    fun capture(index: Int, phase: String) {
                        val beforeReadBegin = SystemClock.elapsedRealtimeNanos()
                        val before = worker.status()
                        val beforeReadEnd = SystemClock.elapsedRealtimeNanos()
                        checkIdentity(before)
                        val captured = worker.debugQmpSnapshot()
                        val diagnostics = JSONObject()
                        before.getBundle("observations")?.let { observed ->
                            @Suppress("DEPRECATION")
                            for (key in listOf("runtimeDiagnosticsAvailable", "engineReady", "displaySleeping", "backlightLevel", "guestAgentStateAvailable",
                                "guestAgentState", "guestAgentStateLabel", "guestAgentEndpointAvailable", "guestAgentFreshnessAgeAvailable", "shutdownPhase", "shutdownRoute",
                                "guestFrameWidth", "guestFrameHeight", "guestFrameSamplePixels", "guestFrameNonzeroRgbSamples", "guestFrameRgbSampleSum"))
                                diagnostics.put(key, observed.get(key) ?: JSONObject.NULL)
                        }
                        val item = JSONObject().put("diagnostics", diagnostics).put("index", index).put("phase", phase).put("error", captured.getString("error"))
                            .put("statusBefore", encode(before))
                            .put("statusBeforeReadBeginElapsedNs", beforeReadBegin).put("statusBeforeReadEndElapsedNs", beforeReadEnd)
                            .put("path", captured.getString("path")).put("engineState", before.getString("state"))
                            .put("touchTraceConfigured", before.getBoolean("touchTraceConfigured"))
                            .put("vcpuAccountingConfigured", before.getBoolean("vcpuAccountingConfigured"))
                            .put("engineLog", before.getString("engineLog")).put("hostElapsedMs", SystemClock.elapsedRealtime())
                        snapshots.put(item); persistSnapshots()
                        assertEquals(captured.toString(), "", captured.getString("error"))
                        val lines = File(requireNotNull(captured.getString("path"))).readLines().map { JSONObject(it) }
                        assertTrue("Missing QMP greeting", lines.first().has("QMP"))
                        val responses = lines.drop(1).associateBy { it.getString("id") }
                        for (id in listOf("lt-cap", "lt-status", "lt-cpus", "lt-registers", "lt-cpu-state", "lt-instructions", "lt-caller")) {
                            val response = requireNotNull(responses[id]); assertFalse(response.toString(), response.has("error"))
                        }
                        item.put("status", responses.getValue("lt-status").getJSONObject("return"))
                            .put("registers", responses.getValue("lt-registers").getString("return"))
                            .put("cpuState", responses.getValue("lt-cpu-state").getString("return"))
                            .put("fixedKernelInstructions", responses.getValue("lt-instructions").getString("return"))
                            .put("fixedCallerInstructions", responses.getValue("lt-caller").getString("return"))
                        val afterReadBegin = SystemClock.elapsedRealtimeNanos()
                        val after = worker.status()
                        val afterReadEnd = SystemClock.elapsedRealtimeNanos()
                        item.put("statusAfter", encode(after))
                            .put("statusAfterReadBeginElapsedNs", afterReadBegin).put("statusAfterReadEndElapsedNs", afterReadEnd)
                        persistSnapshots()
                        checkIdentity(after)
                    }
                    val count = arguments.getString("qmpSamples", "3").toInt().coerceIn(1, 12)
                    repeat(count) { capture(it, "snapshot"); Thread.sleep(3000) }
                    receipt.put("recordId", record).put("diagnosisCaptured", true).put("workerPid", initial.getInt("pid"))
                        .put("appUid", Process.myUid()).put("privateEndpoint", endpoint.absolutePath).put("snapshots", snapshots)
                    val holdMs = holdSeconds * 1000
                    receipt.put("stage", if (holdMs > 0) "holding-foreground" else "complete")
                    persistReceipt()
                    val holdDeadline = SystemClock.elapsedRealtime() + holdMs
                    var index = count
                    while (SystemClock.elapsedRealtime() < holdDeadline) {
                        Thread.sleep(minOf(10_000, (holdDeadline - SystemClock.elapsedRealtime()).coerceAtLeast(1)))
                        capture(index++, "foreground-hold")
                        persistReceipt()
                    }
                    receipt.put("stage", "complete").put("normalInstrumentationMayForceStopOnFinish", true)
                }
            }
        } catch (failure: Throwable) { receipt.put("failure", failure.toString()); throw failure }
        finally { persistReceipt() }
    }
}
