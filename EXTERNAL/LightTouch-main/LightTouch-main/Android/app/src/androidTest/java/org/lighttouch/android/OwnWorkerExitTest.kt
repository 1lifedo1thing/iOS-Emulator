package org.lighttouch.android

import android.os.Build
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class OwnWorkerExitTest {
    @Test fun retainBoundedExitEvidenceForExplicitOwnEnginePid() {
        val pidArgument = InstrumentationRegistry.getArguments().getString("workerPid")
        assumeTrue("Explicit workerPid and API30+ required", pidArgument != null && Build.VERSION.SDK_INT >= 30)
        val pid = requireNotNull(pidArgument).toInt()
        require(pid > 0)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        WorkerExitEvidence.capture(context, pid)
        val directory = File(context.filesDir, "worker-exits")
        val receipt = requireNotNull(directory.listFiles()?.filter { it.name.matches(Regex("engine-$pid-[0-9]+\\.json")) }?.maxByOrNull { it.name }) {
            "No own :engine exit record for PID$pid. Inspect private worker-exits/*unavailable.json or *capture-error.json."
        }
        val metadata = JSONObject(receipt.readText())
        assertEquals(pid, metadata.getInt("pid"))
        assertEquals("${context.packageName}:engine", metadata.getString("process"))
        val result = JSONObject().put("ownWorkerExitCaptured", true).put("pid", pid)
            .put("reason", metadata.getInt("reason")).put("status", metadata.getInt("status"))
            .put("receipt", receipt.absolutePath).put("guestBootProven", false).put("ownNativeTraceRetrieved", false)
        if (metadata.has("tracePath")) {
            val trace = File(metadata.getString("tracePath"))
            assertTrue("Trace exceeded fixed 4MiB cap", trace.length() <= 4 * 1024 * 1024)
            result.put("trace", trace.absolutePath).put("traceBytes", trace.length())
                .put("ownNativeTraceRetrieved", trace.length() > 0).put("traceTruncated", metadata.optBoolean("traceTruncated"))
        }
        if (metadata.optBoolean("traceUnavailable")) result.put("traceUnavailable", true)
        if (metadata.has("traceError")) result.put("traceError", metadata.getString("traceError"))
        // Android may report EXIT_SELF/status1 for QEMU init exit even if a
        // concurrent destructor FORTIFY is logged. Never relabel the platform's
        // exit reason or claim a native trace was retrieved when none is supplied.
        File(directory, "requested-$pid-result.json").writeText(result.toString(2))
    }
}
