package org.lighttouch.android

import android.content.Intent
import android.os.Bundle
import android.os.IBinder
import android.os.Process
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
import java.nio.file.Files
import java.security.MessageDigest
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Checks real normal-start admission of retained interrupted state; never recovers it. */
@RunWith(AndroidJUnit4::class)
class GuestDirtyAdmissionTest {
    @Test fun normalStartRejectsActualDirtyMarkerWithoutChangingRetainedMetadata() {
        val arguments = InstrumentationRegistry.getArguments()
        assumeTrue("Real existing dirty record and explicit test opt-in required", arguments.getString("rejectDirtyStart") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val evidence = File(context.filesDir, "dirty-admission-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("normalDirtyAdmissionRejected", false).put("recoveryInvoked", false)
            .put("guestBootProven", false).put("expectedCoreStatusName", "LT_DIRTY").put("expectedCoreStatus", 6)
            .put("numericNativeStatusObserved", false)
            .put("statusEvidence", "Production Binder exposes error text; exact Storage.cpp LT_DIRTY message checked, numeric status not returned by this API")
        @Suppress("DEPRECATION")
        fun encode(bundle: Bundle): JSONObject = JSONObject().apply {
            for (key in bundle.keySet()) when (val value = bundle.get(key)) { is Bundle -> put(key, encode(value)); else -> put(key, value) }
        }
        fun persist() = File(evidence, "result.json").writeText(result.toString(2))
        try {
            AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                val worker = binding.get()
                val initial = worker.status()
                result.put("initialWorker", encode(initial)); persist()
                assertEquals(Process.myUid(), initial.getInt("uid")); assertNotEquals(Process.myPid(), initial.getInt("pid"))
                assertEquals("Requires a fresh unused worker; test never stops/reuses a guest", "stopped", initial.getString("state"))
                assertFalse(initial.getBoolean("sessionStarted"))
                val records = worker.records()
                val id = arguments.getString("recordId") ?: requireNotNull(records.singleOrNull { it.getBoolean("selected") }?.getString("id"))
                require(id.matches(Regex("[0-9a-f]{32}"))) { "Native library record ID required; arbitrary paths are not accepted" }
                val record = requireNotNull(records.singleOrNull { it.getString("id") == id }) { "Existing native record not found" }
                assertTrue("Record metadata must already report interrupted state; no marker is synthesized", record.getBoolean("dirty"))
                val root = File(context.filesDir, "library/records/$id").canonicalFile
                require(root.toPath().startsWith(context.filesDir.canonicalFile.toPath()))
                val dirty = File(root, "state/.dirty")
                val pin = File(root, "state/.base-identity")
                val files = listOf(File(root, "record.json"), File(root, "base/device.lock.json"), File(root, "base/identity.json"), dirty, pin)
                fun metadata(): JSONObject = JSONObject().apply {
                    for (file in files) {
                        if (file == File(root, "base/identity.json") && !file.exists()) {
                            put(file.relativeTo(root).invariantSeparatorsPath, JSONObject().put("exists", false))
                            continue
                        }
                        require(!Files.isSymbolicLink(file.toPath()) && file.isFile) { "Expected regular retained metadata: ${file.name}" }
                        require(file.length() <= 32 * 1024 * 1024) { "Metadata exceeds fixed read budget" }
                        put(file.relativeTo(root).invariantSeparatorsPath, JSONObject().put("bytes", file.length())
                            .put("modifiedMs", file.lastModified()).put("sha256", acceptanceSha256(file)))
                    }
                }
                // List mutable overlays without copying/reading their potentially large payloads.
                fun overlayMetadata(): JSONObject {
                    val digest = MessageDigest.getInstance("SHA-256")
                    val entries = JSONArray()
                    val state = File(root, "state")
                    val paths = state.walkTopDown().onEnter { directory ->
                        require(!Files.isSymbolicLink(directory.toPath()) && directory.canonicalFile.toPath().startsWith(state.canonicalFile.toPath())) { "Retained state directory link refused" }
                        true
                    }.filter { file ->
                        require(!Files.isSymbolicLink(file.toPath())) { "Retained state symlink refused" }
                        file.isFile && file.name != ".lease"
                    }.take(150_001).toList()
                    require(paths.size <= 150_000) { "Retained overlay listing exceeds fixed entry budget" }
                    for (file in paths.sortedBy { it.relativeTo(state).invariantSeparatorsPath }) {
                        require(!Files.isSymbolicLink(file.toPath())) { "Retained state symlink refused" }
                        val relative = file.relativeTo(state).invariantSeparatorsPath
                        val item = JSONObject().put("path", relative).put("bytes", file.length()).put("modifiedMs", file.lastModified())
                        entries.put(item); digest.update((item.toString() + "\n").toByteArray(Charsets.UTF_8))
                    }
                    return JSONObject().put("files", entries).put("count", paths.size)
                        .put("listingSha256", digest.digest().joinToString("") { "%02x".format(it) }).put("payloadsRehashed", false)
                }
                val before = metadata(); val beforeOverlay = overlayMetadata()
                File(evidence, "metadata-before.json").writeText(before.toString(2))
                File(evidence, "overlay-before.json").writeText(beforeOverlay.toString(2))
                assertEquals("State pin must retain the admitted immutable base identity", acceptanceSha256(File(root, "base/device.lock.json")), pin.readText())
                val marker = dirty.readText()
                assertTrue("Real domain marker schema/identity required", marker.startsWith("schema=1\nbase=" + pin.readText()))
                result.put("recordId", id).put("beforeRecord", encode(record)); persist()
                val binder = worker.asBinder()
                val died = CountDownLatch(1)
                val recipient = IBinder.DeathRecipient { died.countDown() }
                binder.linkToDeath(recipient, 0)
                try {
                    val startedAt = SystemClock.elapsedRealtime()
                    val rejected = worker.start(id) // Normal operation only: fixed allow_dirty=false.
                    result.put("startResult", encode(rejected)).put("normalAdmissionMs", SystemClock.elapsedRealtime() - startedAt); persist()
                    assertEquals("Authoritative core dirty rejection, not generic startup failure",
                        "interrupted session: reopen with explicit cold recovery; data retained", rejected.getString("error"))
                    assertTrue("No guest should start on rejected admission", rejected.getString("state") in listOf("stopped", "failed"))
                    val observations = requireNotNull(rejected.getBundle("observations"))
                    assertEquals("Dirty rejection must precede a ready guest engine", 0L, observations.getLong("engineReady"))
                    assertEquals("No guest frames may be produced on rejected admission", 0L, observations.getLong("producedFrames"))
                    val after = metadata(); val afterOverlay = overlayMetadata()
                    File(evidence, "metadata-after.json").writeText(after.toString(2))
                    File(evidence, "overlay-after.json").writeText(afterOverlay.toString(2))
                    assertEquals("Dirty/base/pin/record metadata changed during rejection", before.toString(), after.toString())
                    assertEquals("Retained overlay listing metadata changed", beforeOverlay.getString("listingSha256"), afterOverlay.getString("listingSha256"))
                    assertEquals(marker, dirty.readText())
                    val disposed = !binder.isBinderAlive || died.await(10, TimeUnit.SECONDS)
                    result.put("failedAdmissionWorkerDisposed", disposed).put("normalDirtyAdmissionRejected", true)
                        .put("retainedMetadataUnchanged", true).put("overlayListingUnchanged", true); persist()
                    assertTrue("Failed start must dispose this worker, not reuse it for recovery", disposed)
                } finally { try { binder.unlinkToDeath(recipient, 0) } catch (_: NoSuchElementException) { } }
            }
        } catch (failure: Throwable) { result.put("failure", failure.toString()); throw failure }
        finally { persist() }
    }
}
