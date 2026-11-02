package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class PerformanceCountersTest {
    private fun counter(wall: Long, cpu: Long, pid: Int = 12, epoch: Long = 1) =
        PerformanceCounters.Counter(pid, epoch, wall, cpu)
    private val observer = PerformanceSamplingGate.Observer(34, 56)

    @Test fun wholeProcessCpuCanExceedOneCoreAndGenuineZeroIsAvailable() {
        val cpu = PerformanceCounters()
        assertNull(cpu.sample(counter(1000, 500)).percent)
        assertEquals(250.0, cpu.sample(counter(3000, 5500)).percent!!, 0.00001)
        val zero = cpu.sample(counter(5000, 5500))
        assertEquals("available", zero.state); assertEquals(0.0, zero.percent!!, 0.0)
    }

    @Test fun cumulativeTimeIsAvailableBeforeRateAndKeepsFractionalSeconds() {
        val cpu = PerformanceCounters()
        assertNull(cpu.sample(counter(1000, 1501)).percent)
        assertEquals(1.501, PerformanceCpuReading.cumulativeSeconds(12, 12, 1501)!!, 0.0)
        assertEquals(0.0, PerformanceCpuReading.cumulativeSeconds(12, 12, 0)!!, 0.0)
        assertTrue(PerformanceCpuReading.cumulativeSeconds(12, 12, Long.MAX_VALUE)!!.isFinite())
    }

    @Test fun missingFailedOrDifferentWorkerCumulativeTimeIsNotReused() {
        assertNull(PerformanceCpuReading.cumulativeSeconds(12, 12, null))
        assertNull(PerformanceCpuReading.cumulativeSeconds(12, 12, -1))
        assertNull(PerformanceCpuReading.cumulativeSeconds(12, 13, 2500))
        assertNull(PerformanceCpuReading.cumulativeSeconds(0, 0, 2500))
        assertEquals(0.025, PerformanceCpuReading.cumulativeSeconds(13, 13, 25)!!, 0.0)
    }

    @Test fun absoluteCpuContinuesAcrossObserverResetWithoutInventedGuestPauseReset() {
        val cpu = PerformanceCounters()
        cpu.sample(counter(1000, 500))
        assertNull(cpu.sample(counter(3000, 950, epoch = 2)).percent)
        assertEquals(0.95, PerformanceCpuReading.cumulativeSeconds(12, 12, 950)!!, 0.0)
        assertEquals(10.0, cpu.sample(counter(5000, 1150, epoch = 2)).percent!!, 0.0)
        assertEquals(1.15, PerformanceCpuReading.cumulativeSeconds(12, 12, 1150)!!, 0.0)
    }

    @Test fun newPidAndNewObservationNeverReuseAnOldCpuBaseline() {
        val cpu = PerformanceCounters()
        cpu.sample(counter(1000, 500))
        assertNull(cpu.sample(counter(3000, 900, pid = 13)).percent)
        assertEquals(10.0, cpu.sample(counter(5000, 1100, pid = 13)).percent!!, 0.00001)
        assertNull(cpu.sample(counter(6000, 1200, pid = 13, epoch = 2)).percent)
        assertEquals(20.0, cpu.sample(counter(8000, 1600, pid = 13, epoch = 2)).percent!!, 0.00001)
    }

    @Test fun nonpositiveTimeAndCounterRegressionsAreNotNegativeOrInfiniteUsage() {
        val cpu = PerformanceCounters()
        cpu.sample(counter(1000, 500))
        assertNull(cpu.sample(counter(1000, 600)).percent)
        assertNull(cpu.sample(counter(900, 650)).percent)
        assertNull(cpu.sample(counter(2000, 100)).percent)
        assertEquals(50.0, cpu.sample(counter(4000, 1100)).percent!!, 0.00001)
    }

    @Test fun unavailableCpuResetsRatherThanPretendingToHaveConsumedZero() {
        val cpu = PerformanceCounters()
        cpu.sample(counter(1000, 500))
        assertEquals("unavailable", cpu.sample(counter(3000, -1)).state)
        assertNull(cpu.sample(counter(5000, 1000)).percent)
        assertEquals(10.0, cpu.sample(counter(7000, 1200)).percent!!, 0.00001)
    }

    @Test fun longSamplingGapWarmsUpButGuestPauseDoesNotChangeHostMeasurement() {
        val cpu = PerformanceCounters()
        cpu.sample(counter(1000, 500))
        // A guest paused during this interval can still consume worker CPU.
        assertEquals(30.0, cpu.sample(counter(3000, 1100)).percent!!, 0.00001)
        assertNull(cpu.sample(counter(20000, 5000)).percent)
        assertEquals(5.0, cpu.sample(counter(22000, 5100)).percent!!, 0.00001)
    }

    @Test fun unfilledMemoryResultIsUnavailableRatherThanFreshZeroPss() {
        // Model the public void wrapper returning without populating MemoryInfo.
        class MemoryResult { var totalPssKiB = 0L }
        val result = MemoryResult()
        val failedVoidRead: (MemoryResult) -> Unit = { /* No fields populated. */ }
        failedVoidRead(result)
        assertNull(PerformanceMemoryReading.workerPssKiB(result.totalPssKiB))
        assertNull(PerformanceMemoryReading.workerPssKiB(-1))
        result.totalPssKiB = 32768
        assertEquals(32768L, PerformanceMemoryReading.workerPssKiB(result.totalPssKiB)!!)
        // A later unfilled read must not reuse the earlier positive result.
        assertNull(PerformanceMemoryReading.workerPssKiB(MemoryResult().totalPssKiB))
    }

    @Test fun frequentCacheRequestsNeverQueueAdditionalMemoryReads() {
        val gate = PerformanceSamplingGate()
        val first = gate.request(observer, 0).ticket!!
        for (time in listOf(1L, 1000L, 2000L, 3000L)) assertNull(gate.request(observer, time).ticket)
        assertTrue(gate.complete(first))
        val second = gate.request(observer, 3001).ticket!!
        assertTrue(gate.complete(second))
        assertNull(gate.request(observer, 5000).ticket)
        assertNotNull(gate.request(observer, 5001).ticket)
    }

    @Test fun observerChangeDuringReadCannotPublishThatOldObserversSample() {
        val gate = PerformanceSamplingGate()
        val old = gate.request(observer, 0)
        val changed = gate.request(observer.copy(token = 57), 1000)
        assertNotEquals(old.epoch, changed.epoch); assertNull(changed.ticket)
        assertFalse(gate.complete(old.ticket!!))
        val fresh = gate.request(observer.copy(token = 57), 2000)
        assertEquals(changed.epoch, fresh.epoch); assertTrue(gate.complete(fresh.ticket!!))
    }

    @Test fun idleWakeAndBackwardsClockStartNewEpochs() {
        val gate = PerformanceSamplingGate()
        val first = gate.request(observer, 0); assertTrue(gate.complete(first.ticket!!))
        val resumed = gate.request(observer, 5000)
        assertNotEquals(first.epoch, resumed.epoch); assertTrue(gate.complete(resumed.ticket!!))
        val backwards = gate.request(observer, 4000)
        assertNotEquals(resumed.epoch, backwards.epoch); assertNotNull(backwards.ticket)
    }

    @Test(timeout = 3000) fun closingWhilePlatformReadIsBlockedDoesNotWaitAndDiscardsLateResult() {
        val gate = PerformanceSamplingGate()
        val ticket = gate.request(observer, 0).ticket!!
        val entered = CountDownLatch(1); val finishRead = CountDownLatch(1)
        var published = true
        val reader = Thread {
            entered.countDown(); finishRead.await()
            published = gate.complete(ticket)
        }
        reader.start()
        try {
            assertTrue(entered.await(1, TimeUnit.SECONDS))
            gate.close() // This must finish before the simulated platform read is released.
            assertNull(gate.request(observer, 10000).ticket)
            finishRead.countDown(); reader.join(1000)
            assertFalse(reader.isAlive); assertFalse(published)
        } finally { finishRead.countDown(); reader.join(1000) }
    }

    @Test fun duplicateOldCompletionCannotClearTheNextReadersSlot() {
        val gate = PerformanceSamplingGate()
        val old = gate.request(observer, 0).ticket!!; assertTrue(gate.complete(old))
        val fresh = gate.request(observer, 2000).ticket!!
        assertFalse(gate.complete(old)); assertNull(gate.request(observer, 5000).ticket)
        assertTrue(gate.complete(fresh)); assertNotNull(gate.request(observer, 5001).ticket)
    }
}
