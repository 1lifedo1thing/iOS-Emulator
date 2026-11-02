package org.lighttouch.android

import android.app.ActivityManager
import android.content.Context
import android.os.Bundle
import android.os.Debug
import android.os.Process
import android.os.SystemClock
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException

/** Demand driven: no timer, native calls, control locks, or platform reads on Binder. */
internal class WorkerPerformanceSampler(context: Context) {
    private val application = context.applicationContext
    private val gate = PerformanceSamplingGate()
    private val counters = PerformanceCounters()
    private val executor = Executors.newSingleThreadExecutor { task ->
        Thread(task, "worker-performance").apply { isDaemon = true }
    }
    private data class Cached(val epoch: Long, val values: Bundle)
    @Volatile private var cached: Cached? = null

    fun request(callerPid: Int, observationEpoch: Long): Bundle {
        val demand = gate.request(PerformanceSamplingGate.Observer(callerPid, observationEpoch), SystemClock.elapsedRealtime())
        demand.ticket?.let { ticket ->
            try {
                executor.execute {
                    val result = collect(ticket)
                    // Request/complete are metadata-only; synchronize publication with
                    // a concurrent observer change, and re-check epoch on every read.
                    if (gate.complete(ticket)) cached = Cached(ticket.epoch, result)
                }
            } catch (_: RejectedExecutionException) { gate.complete(ticket) }
        }
        val sample = cached?.takeIf { it.epoch == demand.epoch }?.values
        return (sample?.let { Bundle(it) } ?: Bundle().apply {
            putInt("pid", Process.myPid()); putString("cpuState", "warming up")
        }).apply {
            putLong("samplingEpoch", demand.epoch)
            putString("deviceCpuState", "unavailable")
            putString("guestCpuState", "unavailable")
            putString("guestMemoryState", "unavailable")
        }
    }

    private fun collect(ticket: PerformanceSamplingGate.Ticket): Bundle {
        val begin = SystemClock.elapsedRealtime()
        val cpuTime = try { Process.getElapsedCpuTime() } catch (_: RuntimeException) { -1L }
        val cpu = counters.sample(PerformanceCounters.Counter(Process.myPid(), ticket.epoch, begin, cpuTime))
        val values = Bundle().apply {
            putInt("pid", Process.myPid()); putInt("uid", Process.myUid())
            putLong("cpuSampleElapsedMs", begin); putLong("processCpuMs", cpuTime)
            putString("cpuState", cpu.state)
            cpu.percent?.let { putDouble("workerCpuPercentOfOneCore", it) }
        }
        try {
            val memory = Debug.MemoryInfo()
            Debug.getMemoryInfo(memory)
            val pss = PerformanceMemoryReading.workerPssKiB(memory.totalPss.toLong())
            if (pss != null) {
                values.putLong("workerPssKiB", pss)
                values.putLong("pssSampleElapsedMs", SystemClock.elapsedRealtime())
            }
        } catch (_: RuntimeException) { /* Unavailable; never substitute zero. */ }
        try {
            val manager = application.getSystemService(ActivityManager::class.java)
            val memory = ActivityManager.MemoryInfo()
            manager?.getMemoryInfo(memory)
            if (manager != null && memory.totalMem > 0 && memory.availMem in 0..memory.totalMem) {
                values.putLong("deviceAvailableBytes", memory.availMem)
                values.putLong("deviceTotalBytes", memory.totalMem)
                values.putLong("deviceMemorySampleElapsedMs", SystemClock.elapsedRealtime())
            }
        } catch (_: RuntimeException) { /* No privileged fallback. */ }
        values.putLong("collectionCompletedElapsedMs", SystemClock.elapsedRealtime())
        values.putLong("collectionCostMs", SystemClock.elapsedRealtime() - begin)
        return values
    }

    fun close() { gate.close(); cached = null; executor.shutdownNow() } // Never join a platform read.
}
