package org.lighttouch.android

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.os.DeadObjectException
import android.os.IBinder
import android.os.Process
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Observes an existing worker only; an operator independently requests production Stop. */
@RunWith(AndroidJUnit4::class)
class GuestShutdownProgressTest {
    @Test fun observeCachedProgressWithoutCallingNativeOrRequestingStop() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit shutdown observer opt-in required", arguments.getString("observeShutdownProgress") == "true")
        val expectedApk = requireNotNull(arguments.getString("expectedMainApkSha256"))
        require(expectedApk.matches(Regex("[a-f0-9]{64}")))
        val expectedPid = requireNotNull(arguments.getString("expectedWorkerPid")?.toIntOrNull()).also { require(it > 0) }
        val expectedStart = requireNotNull(arguments.getString("expectedWorkerStartTicks")).also { require(it.toLongOrNull()?.let { ticks -> ticks > 0 } == true) }
        val expectedBoot = requireNotNull(arguments.getString("expectedBootId")).also { require(it.matches(Regex("[a-f0-9-]{36}"))) }
        val seconds = (arguments.getString("shutdownProgressSeconds") ?: "40").toLong().also { require(it in 1..45) }
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        assertNotEquals("A separate existing worker is required", Process.myPid(), expectedPid)
        assertTrue("Debug build required", context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0)
        assertEquals("Installed main APK identity", expectedApk, acceptanceSha256(File(context.applicationInfo.sourceDir)))
        assertEquals("Android boot identity", expectedBoot, File("/proc/sys/kernel/random/boot_id").readText().trim())
        val evidence = File(context.filesDir, "shutdown-progress-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("guestAcceptance", false).put("shutdownConfirmationInferred", false)
            .put("terminalReceiptAuthority", false).put("startStopResumeRecoveryRequestedByCollector", false)
            .put("mainApkSha256", expectedApk).put("expectedWorkerPid", expectedPid).put("expectedStartTicks", expectedStart)
            .put("androidBootId", expectedBoot)
            .put("expectedWorkerUid", Process.myUid()).put("clock", "android.SystemClock.elapsedRealtime/BOOTTIME")
            .put("requestedSeconds", seconds).put("collectionComplete", false)
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) {
                is Bundle -> put(key, encode(value))
                else -> put(key, value)
            }
        }
        fun identity(): Boolean {
            val stat = File("/proc/$expectedPid/stat")
            if (!stat.exists()) return false
            val fields = stat.readText().substringAfterLast(')').trim().split(Regex("\\s+"))
            assertEquals("Worker PID was reused", expectedStart, fields[19]) // stat field22; suffix starts at field3.
            val uid = File("/proc/$expectedPid/status").useLines { lines -> lines.first { it.startsWith("Uid:") } }
                .substringAfter(':').trim().split(Regex("\\s+"))[0].toInt()
            assertEquals("Worker UID changed", Process.myUid(), uid)
            return true
        }
        val connected = CountDownLatch(1)
        var worker: IEngine? = null
        var bound = false
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) { worker = IEngine.Stub.asInterface(binder); connected.countDown() }
            override fun onServiceDisconnected(name: ComponentName) = Unit
        }
        try {
            assertTrue("Expected existing worker is absent", identity())
            // No BIND_AUTO_CREATE, foreground service start, ActivityScenario or Native.status call.
            bound = context.bindService(Intent(context, EngineService::class.java), connection, 0)
            assertTrue("Existing worker bind failed", bound)
            assertTrue("Existing worker connection timed out", connected.await(5, TimeUnit.SECONDS))
            val existing = requireNotNull(worker)
            val began = SystemClock.elapsedRealtime()
            val deadline = began + seconds * 1000
            var samples = 0
            var revision = -1L
            File(evidence, "samples.jsonl").bufferedWriter().use { output ->
                while (SystemClock.elapsedRealtime() < deadline && samples < 180) {
                    if (!existing.asBinder().isBinderAlive || !identity()) {
                        result.put("workerDisappearanceObserved", true); break
                    }
                    val before = SystemClock.elapsedRealtimeNanos()
                    // This is the ONLY worker method invoked by this collector.
                    val progress = existing.debugShutdownProgress()
                    val after = SystemClock.elapsedRealtimeNanos()
                    assertEquals(expectedPid, progress.getInt("workerPid")); assertEquals(Process.myUid(), progress.getInt("workerUid"))
                    assertTrue(progress.getBoolean("cachedJavaProgressOnly")); assertFalse(progress.getBoolean("terminalReceiptAuthority"))
                    assertFalse(progress.getBoolean("shutdownConfirmationInferred"))
                    assertTrue("Cached revision regressed", progress.getLong("revision") >= revision)
                    revision = progress.getLong("revision")
                    val sample = JSONObject().put("readBeginElapsedNs", before).put("readEndElapsedNs", after)
                        .put("returnedAfterCollectorDeadline", after / 1_000_000 >= deadline)
                        .put("progress", encode(progress))
                    output.append(sample.toString()).append('\n'); output.flush()
                    samples++
                    result.put("samples", samples).put("lastProgress", encode(progress))
                    val remaining = deadline - SystemClock.elapsedRealtime()
                    if (remaining > 0) Thread.sleep(minOf(250L, remaining))
                }
            }
            assertTrue("No progress samples returned", samples > 0)
            result.put("collectionComplete", true).put("endedAtHostMs", SystemClock.elapsedRealtime())
        } catch (death: DeadObjectException) {
            // A dead Binder is not proof of clean guest shutdown; verify the independent terminal receipt.
            result.put("workerBinderDeathObserved", true).put("collectionEndedByBinderDeath", true)
        } catch (failure: Throwable) { result.put("failure", failure.toString()); throw failure }
        finally {
            if (bound) context.unbindService(connection)
            File(evidence, "result.json").writeText(result.toString(2))
        }
    }
}
