package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

class WorkerStartAdmissionTest {
    @Test fun failedConfigurationDoesNotConsumeAndConsumedWorkerNeverConfigures() {
        val admission = WorkerStartAdmission()
        var calls = 0
        assertEquals("actual configuration failure", admission.configureUnused { calls++; "actual configuration failure" })
        assertFalse(admission.isConsumed())
        assertEquals("", admission.configureUnused { calls++; "" })
        assertEquals(2, calls)
        assertTrue(admission.tryConsume())
        assertTrue(admission.isConsumed())
        assertTrue(admission.configureUnused { calls++; "" }.isNotEmpty())
        assertEquals(2, calls)
        assertFalse(admission.tryConsume()) // A failed Native.start cannot restore this worker.
    }

    @Test fun startCannotCrossAnInFlightConfigurationAndLateArmCannotRun() {
        val admission = WorkerStartAdmission()
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val attempting = CountDownLatch(1); val consumed = CountDownLatch(1)
        val failures = AtomicReference<Throwable?>()
        val order = mutableListOf<String>()
        val arm = Thread {
            try {
                assertEquals("", admission.configureUnused {
                    entered.countDown(); check(release.await(2, TimeUnit.SECONDS))
                    order.add("configured"); ""
                })
            } catch (failure: Throwable) { failures.set(failure) }
        }
        val start = Thread {
            try {
                attempting.countDown()
                assertTrue(admission.tryConsume()); order.add("consumed"); consumed.countDown()
            } catch (failure: Throwable) { failures.set(failure) }
        }
        try {
            arm.start(); assertTrue(entered.await(2, TimeUnit.SECONDS)); start.start()
            assertTrue(attempting.await(2, TimeUnit.SECONDS))
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2)
            while (start.state != Thread.State.BLOCKED && start.isAlive && System.nanoTime() < deadline) Thread.yield()
            assertEquals("Actual start admission must wait for configuration commit", Thread.State.BLOCKED, start.state)
            assertEquals(1L, consumed.count)
            release.countDown(); assertTrue(consumed.await(2, TimeUnit.SECONDS))
            assertTrue(admission.configureUnused { error("Late arm callback executed") }.isNotEmpty())
        } finally {
            release.countDown(); arm.join(2500); if (start.state != Thread.State.NEW) start.join(2500)
        }
        assertFalse(arm.isAlive); assertFalse(start.isAlive)
        failures.get()?.let { throw AssertionError("Admission thread failed", it) }
        assertEquals(listOf("configured", "consumed"), order)
    }
}
