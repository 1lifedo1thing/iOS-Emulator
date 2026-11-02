package org.lighttouch.android

/** Whole-process CPU, normalized to one core. No guest-state input or 100% clamp. */
internal class PerformanceCounters(private val maximumGapMs: Long = 10_000) {
    data class Counter(val pid: Int, val epoch: Long, val elapsedMs: Long, val cpuMs: Long)
    data class Reading(val percent: Double? = null, val state: String = "warming up")
    private var previous: Counter? = null

    fun sample(current: Counter): Reading {
        val old = previous
        if (current.pid <= 0 || current.elapsedMs < 0 || current.cpuMs < 0) {
            previous = null
            return Reading(state = "unavailable")
        }
        previous = current
        if (old == null || current.pid != old.pid || current.epoch != old.epoch)
            return Reading()
        if (current.elapsedMs <= old.elapsedMs || current.cpuMs < old.cpuMs)
            return Reading(state = "warming up (counter reset)")
        val elapsed = current.elapsedMs - old.elapsedMs
        if (elapsed > maximumGapMs) return Reading(state = "warming up (sampling resumed)")
        return Reading(100.0 * (current.cpuMs - old.cpuMs).toDouble() / elapsed, "available")
    }
}

/** The public void memory read can leave an unfilled zero-valued result. */
internal object PerformanceMemoryReading {
    fun workerPssKiB(reportedKiB: Long): Long? = reportedKiB.takeIf { it > 0 }
}

/** Absolute current-process CPU time, independent of rate baseline/guest state. */
internal object PerformanceCpuReading {
    fun cumulativeSeconds(samplePid: Int, currentPid: Int, processCpuMs: Long?): Double? {
        if (currentPid <= 0 || samplePid != currentPid || processCpuMs == null || processCpuMs < 0) return null
        return processCpuMs / 1_000.0
    }
}

/** Tiny metadata-only monitor; platform memory reads always occur outside it. */
internal class PerformanceSamplingGate(
    private val cadenceMs: Long = 2_000,
    private val idleGapMs: Long = 4_000
) {
    data class Observer(val callerPid: Int, val token: Long)
    data class Ticket(val epoch: Long, val startedMs: Long)
    data class Demand(val epoch: Long, val ticket: Ticket?)
    private var observer: Observer? = null
    private var lastDemandMs: Long? = null
    private var lastStartedMs: Long? = null
    private var inFlight: Ticket? = null
    private var epoch = 0L
    private var closed = false

    @Synchronized fun request(who: Observer, nowMs: Long): Demand {
        if (closed) return Demand(epoch, null)
        val last = lastDemandMs
        if (observer != who || last == null || nowMs < last || nowMs - last > idleGapMs) {
            epoch++
            observer = who
        }
        lastDemandMs = nowMs
        val started = lastStartedMs
        val due = started == null || nowMs < started || nowMs - started >= cadenceMs
        val ticket = if (inFlight == null && due) Ticket(epoch, nowMs) else null
        if (ticket != null) { inFlight = ticket; lastStartedMs = nowMs }
        return Demand(epoch, ticket)
    }

    @Synchronized fun complete(ticket: Ticket): Boolean {
        if (inFlight != ticket) return false
        inFlight = null
        return !closed && ticket.epoch == epoch
    }

    @Synchronized fun close() { closed = true; epoch++ }
}
