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
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipFile

/** Frame evidence only: a coherent lit logo is not a verified guest scene. */
internal fun angleFrameReady(state: Bundle, requireNonzero: Boolean): Boolean {
    val io = state.getBundle("observations") ?: return false
    val now = io.getLong("hostMonotonicNs")
    val published = io.getLong("framePublishedHostNs")
    return state.getString("state") == "running" &&
        io.getBoolean("runtimeDiagnosticsAvailable") && io.getLong("engineReady") == 1L &&
        io.getLong("displaySleeping") == 0L && io.getLong("backlightLevel") > 0L &&
        io.getLong("guestFrameWidth") > 0L && io.getLong("guestFrameHeight") > 0L &&
        io.getLong("guestFrameSamplePixels") > 0L &&
        (!requireNonzero || io.getLong("guestFrameNonzeroRgbSamples") > 0L) &&
        io.getLong("guestFrameSequence") > 0L &&
        io.getLong("guestFrameSequence") == io.getLong("producerFrameSequence") &&
        io.getLong("guestFrameTimestampNs") > 0L &&
        io.getLong("guestFrameTimestampNs") == io.getLong("producerFrameTimestampNs") &&
        published > 0L && now >= published && now - published <= 5_000_000_000L
}

internal fun anglePollSleep(deadlineMs: Long, nowMs: Long, intervalMs: Long): Long =
    if (nowMs >= deadlineMs) 0L else minOf(intervalMs, deadlineMs - nowMs)

internal fun angleFrameReadyInTime(state: Bundle, requireNonzero: Boolean, observedAtMs: Long, deadlineMs: Long): Boolean =
    observedAtMs < deadlineMs && angleFrameReady(state, requireNonzero)

/** A real normal-app-UID provider exercise; no arbitrary environment or library path. */
@RunWith(AndroidJUnit4::class)
class GuestAngleProviderTest {
    @Test fun fixedPackagedProviderStartsInFreshGuestWorker() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit packaged ANGLE guest opt-in required", arguments.getString("exerciseAngleGuest") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val abi = android.os.Build.SUPPORTED_ABIS[0]
        val providerManifest = when (abi) {
            "x86_64" -> "providers/angle-runtime.json"
            "arm64-v8a" -> "providers/angle-runtime-arm64-v8a.json"
            else -> error("No packaged debug ANGLE provider for host ABI $abi")
        }
        val recoveryArgument = arguments.getString("coldRecover", "false")
        require(recoveryArgument in listOf("true", "false"))
        val coldRecover = recoveryArgument == "true"
        val evidence = File(context.filesDir, "angle-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val receipt = JSONObject().put("providerDispatchObserved", false).put("guestAcceptancePassed", false)
            .put("visibleGuestVerified", false).put("audiblePlaybackVerified", false)
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
            File(evidence, "result.json").writeText(receipt.toString(2))
            File(evidence, "observations.json").writeText(timeline.toString(2))
        }
        try {
            val manifest = JSONObject(context.assets.open(providerManifest).bufferedReader().use { it.readText() })
            assertEquals("Provider host ABI", abi, manifest.getString("abi"))
            receipt.put("hostAbi", abi).put("coldRecoveryRequested", coldRecover)
                .put("runtimeManifest", manifest).put("installedApkSha256", acceptanceSha256(File(context.applicationInfo.sourceDir)))
            ZipFile(context.applicationInfo.sourceDir).use { apk ->
                val libraries = manifest.getJSONObject("libraries")
                for (name in listOf("libEGL_lighttouch_angle.so", "libGLESv1_CM_lighttouch_angle.so", "libGLESv2_lighttouch_angle.so")) {
                    val entry = requireNotNull(apk.getEntry("lib/$abi/$name"))
                    val digest = MessageDigest.getInstance("SHA-256")
                    apk.getInputStream(entry).use { input -> val bytes = ByteArray(65536); while (true) { val n = input.read(bytes); if (n < 0) break; digest.update(bytes, 0, n) } }
                    assertEquals("Packaged provider identity", libraries.getJSONObject(name).getString("sha256"), digest.digest().joinToString("") { "%02x".format(it) })
                }
            }
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                    val worker = binding.get()
                    val initial = worker.status()
                    assertEquals(Process.myUid(), initial.getInt("uid")); assertNotEquals(Process.myPid(), initial.getInt("pid"))
                    assertFalse("Root must dispose any consumed worker first", initial.getBoolean("sessionStarted"))
                    assertEquals("stopped", initial.getString("state"))
                    assertTrue("Unknown choice was accepted", worker.debugSelectGraphicsProvider(99).isNotEmpty())
                    assertEquals("Fixed provider library preflight", "", worker.debugSelectGraphicsProvider(1))
                    assertTrue("Provider choice must be immutable even before start", worker.debugSelectGraphicsProvider(0).isNotEmpty())
                    val selected = worker.status()
                    assertEquals("angle-gl", selected.getString("graphicsProviderChoice")); assertTrue(selected.getBoolean("graphicsProviderFixed"))
                    val record = arguments.getString("recordId") ?: requireNotNull(worker.records().firstOrNull { it.getBoolean("selected") }?.getString("id"))
                    scenario.onActivity { it.startForegroundService(Intent(it, EngineService::class.java)) }
                    val started = if (coldRecover) worker.recoverStart(record) else worker.start(record)
                    assertEquals(started.toString(), "", started.getString("error"))
                    assertTrue("Provider setter admitted after start", worker.debugSelectGraphicsProvider(1).isNotEmpty())
                    receipt.put("workerPid", started.getInt("pid")).put("appUid", Process.myUid()).put("recordId", record)
                    val log = File(requireNotNull(started.getString("engineLog")))
                    val seconds = arguments.getString("angleObserveSeconds", "180").toLong().coerceIn(10, 1200)
                    val deadline = SystemClock.elapsedRealtime() + seconds * 1000
                    val requireNonzero = arguments.getString("requireNonzeroGuestFrame") == "true"
                    receipt.put("observationDeadlineElapsedMs", deadline).put("nonzeroFrameRequired", requireNonzero)
                    val logPrefix = ByteArrayOutputStream()
                    fun prefix(): String {
                        // Read each retained byte at most once, including during
                        // slow guest boot; never scan unrelated/process-wide logs.
                        java.io.RandomAccessFile(log, "r").use { input ->
                            input.seek(logPrefix.size().toLong())
                            val bytes = ByteArray(8192)
                            while (logPrefix.size() < 4 * 1024 * 1024) {
                                val n = input.read(bytes, 0, minOf(bytes.size, 4 * 1024 * 1024 - logPrefix.size()))
                                if (n < 0) break
                                logPrefix.write(bytes, 0, n)
                            }
                        }
                        return logPrefix.toByteArray().toString(Charsets.UTF_8)
                    }
                    var providerLog = ""
                    do {
                        val state = worker.status()
                        assertEquals("Worker changed", started.getInt("pid"), state.getInt("pid"))
                        timeline.put(JSONObject().put("stage", "provider").put("hostElapsedMs", SystemClock.elapsedRealtime()).put("status", encode(state))); persist()
                        check(state.getString("state") !in listOf("failed", "stopped")) { state.toString() }
                        providerLog = prefix()
                        if (providerLog.contains("[gles-egl] provider=angle-gl; verified public EGL/GLES dispatch")) break
                        val sleep = anglePollSleep(deadline, SystemClock.elapsedRealtime(), 2000)
                        if (sleep > 0) Thread.sleep(sleep)
                    } while (SystemClock.elapsedRealtime() < deadline)
                    File(evidence, "provider-log-prefix.txt").writeText(providerLog)
                    receipt.put("engineLog", log.absolutePath).put("logPrefixLimitBytes", 4 * 1024 * 1024)
                        .put("logPrefixTruncated", log.length() > 4 * 1024 * 1024).put("observationSeconds", seconds)
                    val ownerLines = providerLog.lineSequence().filter { it.startsWith("[gles-egl] public entry ") }.toList()
                    receipt.put("entryOwnerLines", JSONArray(ownerLines))
                    assertTrue("Actual engine did not confirm the fixed provider; old engine candidates are insufficient", providerLog.contains("[gles-egl] provider=angle-gl; verified public EGL/GLES dispatch"))
                    assertTrue("Actual public address-owner checks missing", ownerLines.size >= 197)
                    receipt.put("providerDispatchObserved", true).put("providerVerifiedElapsedMs", SystemClock.elapsedRealtime())
                    persist()

                    // Reuse the original deadline. A current coherent static frame
                    // is sufficient; no new serial or guest scene is manufactured.
                    var frameReady = false
                    var lastFrameState: Bundle? = null
                    var frameFailure = ""
                    var nextPersist = 0L
                    while (SystemClock.elapsedRealtime() < deadline) {
                        try {
                            val state = worker.status()
                            lastFrameState = state
                            val observedAt = SystemClock.elapsedRealtime()
                            check(state.getInt("pid") == started.getInt("pid")) { "Worker changed during frame readiness" }
                            check(state.getString("state") !in listOf("failed", "stopped")) { state.toString() }
                            frameReady = angleFrameReadyInTime(state, requireNonzero, observedAt, deadline)
                            if (observedAt >= nextPersist || frameReady || observedAt >= deadline) {
                                timeline.put(JSONObject().put("stage", "frame-ready").put("hostElapsedMs", observedAt).put("status", encode(state)))
                                persist()
                                nextPersist = observedAt + 2000
                            }
                            if (frameReady) break
                            if (observedAt >= deadline) {
                                frameFailure = "Original observation deadline elapsed during status"
                                break
                            }
                        } catch (failure: Throwable) {
                            frameFailure = failure.toString()
                            break
                        }
                        val sleep = anglePollSleep(deadline, SystemClock.elapsedRealtime(), 250)
                        if (sleep > 0) Thread.sleep(sleep)
                    }
                    receipt.put("frameReadyObserved", frameReady).put("frameReadinessEndedElapsedMs", SystemClock.elapsedRealtime())
                        .put("frameReadinessFailure", frameFailure).put("framePublishMaximumAgeNs", 5_000_000_000L)
                    lastFrameState?.let { receipt.put("frameReadinessWorker", encode(it)) }
                    persist()

                    // Preserve the actual capture/error even when readiness failed.
                    // Capture diagnostics do not start another readiness interval.
                    val image = File(context.filesDir, "screenshots/${evidence.name}/provider.png")
                    receipt.put("capture", image.absolutePath).put("captureStartedElapsedMs", SystemClock.elapsedRealtime())
                        .put("postCaptureFrameReadyObserved", false)
                    persist()
                    val captureError = try { worker.screenshot(image.absolutePath) } catch (failure: Throwable) {
                        receipt.put("captureThrowable", failure.toString())
                        failure.toString()
                    }
                    receipt.put("captureError", captureError).put("captureEndedElapsedMs", SystemClock.elapsedRealtime())
                    if (image.isFile) receipt.put("captureBytes", image.length()).put("captureSha256", acceptanceSha256(image))
                    var postCaptureFrameReady = false
                    try {
                        val terminal = worker.status()
                        val observedAt = SystemClock.elapsedRealtime()
                        receipt.put("terminalWorker", encode(terminal)).put("postCaptureObservedElapsedMs", observedAt)
                        postCaptureFrameReady = terminal.getInt("pid") == started.getInt("pid") &&
                            angleFrameReadyInTime(terminal, requireNonzero, observedAt, deadline)
                    } catch (failure: Throwable) {
                        receipt.put("terminalWorkerFailure", failure.toString())
                    }
                    receipt.put("postCaptureFrameReadyObserved", postCaptureFrameReady)
                    persist()
                    assertTrue("Provider verified, but no fresh coherent copied lit frame within the original observation deadline: $frameFailure", frameReady)
                    assertEquals("Actual copied guest frame capture", "", captureError)
                    assertTrue("Copied frame was not current, coherent and lit after capture within the original observation deadline; raw capture retained", postCaptureFrameReady)
                }
            }
        } catch (failure: Throwable) { receipt.put("failure", failure.toString()); throw failure }
        finally { persist() }
    }
}
