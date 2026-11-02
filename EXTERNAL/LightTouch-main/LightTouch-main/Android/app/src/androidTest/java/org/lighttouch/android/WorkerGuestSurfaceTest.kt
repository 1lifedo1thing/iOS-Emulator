package org.lighttouch.android

import android.content.Intent
import android.os.Binder
import android.os.SystemClock
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.lang.reflect.Proxy

/** Actual Compose/SurfaceView lifecycle with fake workers; no guest or dead-process
 * producer acceptance. Requires an unused real service before replacing its UI. */
@RunWith(AndroidJUnit4::class)
class WorkerGuestSurfaceTest {
    private class Endpoint {
        val identity = Binder()
        val calls = mutableListOf<String>()
        val worker = Proxy.newProxyInstance(IEngine::class.java.classLoader, arrayOf(IEngine::class.java)) { proxy, method, arguments ->
            when (method.name) {
                // Compose's structural state policy invokes Object.equals when
                // selecting another worker. Object methods are not Binder calls.
                "equals" -> proxy === arguments?.get(0)
                "hashCode" -> System.identityHashCode(proxy)
                "toString" -> "Fake Surface worker@${System.identityHashCode(proxy).toString(16)}"
                "asBinder" -> identity
                "claimDisplay" -> { calls += "claim"; 1L }
                "attachDisplay" -> { calls += "attach"; "" }
                "releaseDisplay" -> { calls += "release"; null }
                "detachDisplay" -> { calls += "detach"; null }
                "cancelDisplayInput" -> { calls += "cancel"; null }
                "backgroundDisplay" -> { calls += "background"; null }
                else -> error("Unexpected Surface control: ${method.name}")
            }
        } as IEngine
    }
    @Test fun newWorkerCreatesNewSurfaceViewAndRetiredCallbacksCannotReachIt() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("Explicit isolated Surface test required", args.getString("exerciseWorkerSurfaceReplacement") == "true")
        val context = instrumentation.targetContext
        val expected = requireNotNull(args.getString("expectedMainApkSha256"))
        require(expected.matches(Regex("[0-9a-f]{64}")))
        assertEquals(expected, acceptanceSha256(File(context.applicationInfo.sourceDir)))
        val evidence = File(context.filesDir, "surface-worker-evidence-${System.currentTimeMillis()}").apply { check(mkdir()) }
        val result = JSONObject().put("guestAcceptancePassed", false).put("crossProcessProducerRecoveryProven", false)
        try {
            AcceptanceBinding(context, Intent(context, EngineService::class.java), IEngine.Stub::asInterface).use { binding ->
                val initial = binding.get().status()
                assertFalse("No live/consumed guest may be replaced by this test", initial.getBoolean("sessionStarted"))
                assertEquals("stopped", initial.getString("state"))
                assertEquals("", initial.getString("error"))
                val a = Endpoint(); val b = Endpoint()
                // Exercise the proxy's actual Object methods before Compose:
                // selecting B must compare unequal without touching Binder.
                assertTrue(a.worker.equals(a.worker))
                assertFalse(a.worker.equals(b.worker))
                assertFalse(a.worker.equals(null))
                assertEquals(System.identityHashCode(a.worker), a.worker.hashCode())
                assertTrue(a.worker.toString().startsWith("Fake Surface worker@"))
                assertTrue(a.calls.isEmpty() && b.calls.isEmpty())
                val selected = mutableStateOf<IEngine?>(a.worker)
                val screenOn = mutableStateOf(false)
                var latest: GuestDisplay? = null
                val created = mutableListOf<GuestDisplay>()
                val released = mutableListOf<GuestDisplay>()
                val errors = mutableListOf<String>()
                ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                    fun ui(action: (MainActivity) -> Unit) {
                        var failure: Throwable? = null
                        scenario.onActivity { activity ->
                            try { action(activity) } catch (caught: Throwable) { failure = caught }
                        }
                        failure?.let { throw it } // Fail JUnit, never the application's Looper.
                    }
                    ui { activity -> activity.setContent {
                        WorkerGuestSurface(worker = selected.value, currentWorker = { selected.value },
                            mayClaim = { latest === it }, ownerTicket = { 1L }, onError = { errors += it },
                            onCreated = { latest = it; created += it }, onReleased = { released += it },
                            keepScreenOn = screenOn.value, modifier = Modifier.fillMaxSize())
                    } }
                    fun awaitSurface(different: GuestDisplay? = null): GuestDisplay {
                        val deadline = SystemClock.uptimeMillis() + 5000
                        val expectedEndpoint = if (different == null) a else b
                        while (SystemClock.uptimeMillis() < deadline) {
                            var ready: GuestDisplay? = null
                            ui {
                                latest?.takeIf { it !== different && it.holder.surface.isValid && "attach" in expectedEndpoint.calls }?.let { ready = it }
                            }
                            ready?.let { return it }
                            SystemClock.sleep(20)
                        }
                        error("Fresh actual SurfaceView was not ready within5s")
                    }
                    val first = awaitSurface()
                    ui { screenOn.value = true }
                    instrumentation.waitForIdleSync()
                    ui {
                        assertSame("Same worker recomposition must retain its View", first, latest)
                        assertEquals(1, created.size)
                        selected.value = b.worker
                        // This callback runs before Compose can replace A. It must
                        // not attach A's old holder/queue to newly selected B.
                        val oldBefore = a.calls.size
                        first.attachActiveSurface()
                        first.cancelOwnedGesture()
                        first.surfaceDestroyed(first.holder)
                        assertEquals("Cleanup must retain A's captured worker", listOf("cancel", "detach"), a.calls.drop(oldBefore))
                        assertTrue(b.calls.isEmpty())
                    }
                    val second = awaitSurface(first)
                    ui {
                        assertNotSame(first, second)
                        assertNotSame(first.holder, second.holder)
                        assertEquals(2, created.size)
                        assertTrue("A must be disposed, not pooled/reused", first in released)
                        assertEquals("Disposal must release only A's captured lease exactly once", 1, a.calls.count { it == "release" })
                        assertTrue(second.holder.surface.isValid)
                        val before = b.calls.toList()
                        first.surfaceDestroyed(first.holder)
                        first.onWindowFocusChanged(true)
                        first.cancelOwnedGesture()
                        first.backgroundOwnedDisplay()
                        first.retireDisplay()
                        assertEquals("Late old callbacks reached new worker", before, b.calls)
                        assertTrue(errors.isEmpty())
                    }
                    result.put("sameWorkerViewRetained", true).put("newWorkerViewAndHolderReplaced", true)
                        .put("lateCallbacksSuppressed", true).put("actualNewSurfaceValid", true)
                }
            }
        } catch (failure: Throwable) {
            result.put("failure", failure.message ?: failure.javaClass.name)
            throw failure
        } finally {
            File(evidence, "result.json").writeText(result.toString(2))
        }
    }
}
