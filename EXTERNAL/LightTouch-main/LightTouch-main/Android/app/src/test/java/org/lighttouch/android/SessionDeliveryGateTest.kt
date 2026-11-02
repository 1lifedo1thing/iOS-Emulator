package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class SessionDeliveryGateTest {
    @Test fun delayedOldReceiptCannotOverwriteOrClearNewWorkerError() {
        for (oldReceipt in listOf("", "Old worker timed out")) {
            val gate = SessionDeliveryGate()
            val retiredWorker = gate.advance()
            val reading = CountDownLatch(1)
            val releaseRead = CountDownLatch(1)
            val returned = CountDownLatch(1)
            val deliveries = LinkedBlockingQueue<() -> Boolean>()
            var liveError = "Old worker disposed"
            val io = Thread {
                reading.countDown()
                try {
                    check(releaseRead.await(3, TimeUnit.SECONDS))
                    deliveries.put { gate.deliver(retiredWorker) { liveError = oldReceipt } }
                } finally { returned.countDown() }
            }.apply { start() }
            try {
                assertTrue(reading.await(1, TimeUnit.SECONDS))
                gate.advance() // New worker binds/starts while old receipt IO is blocked.
                liveError = "New worker startup failure"
                releaseRead.countDown()
                val delivery = requireNotNull(deliveries.poll(1, TimeUnit.SECONDS))
                assertFalse("A retired worker delivered into the replacement session", delivery())
                assertEquals("New worker startup failure", liveError)
            } finally { releaseRead.countDown(); io.join(3000) }
            assertTrue(returned.await(1, TimeUnit.SECONDS)); assertFalse(io.isAlive)
        }
    }
    @Test fun duplicateDeathAndDisconnectPreserveFirstPidReceipt() {
        val gate = SessionDeliveryGate()
        gate.advance() // Connected worker.
        val ownExit = requireNotNull(gate.retire(workerPresent = true))
        assertNull(gate.retire(workerPresent = false)) // Second callback sees cleared identity.
        var error = "Confirmation unavailable"
        assertTrue(gate.deliver(ownExit) { error = "" })
        assertEquals("", error)
        gate.advance() // A replacement worker still invalidates that receipt.
        assertFalse(gate.deliver(ownExit) { error = "Old timeout" })
        assertEquals("", error)
    }
    @Test fun currentTerminalReceiptStillClearsOnlyItsOwnError() {
        val gate = SessionDeliveryGate()
        val ownExit = gate.advance()
        var error = "Shutdown confirmation unavailable"
        assertTrue(gate.deliver(ownExit) { error = "" })
        assertEquals("", error)
        gate.advance() // Activity destroyed or later worker retirement.
        assertFalse(gate.deliver(ownExit) { error = "Late failure" })
        assertEquals("", error)
    }
}
