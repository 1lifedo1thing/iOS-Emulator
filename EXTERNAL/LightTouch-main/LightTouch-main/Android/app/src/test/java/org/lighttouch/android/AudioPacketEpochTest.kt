package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicBoolean

/** Sink lifetime tests; symbolic writes are not synthetic guest/audio acceptance. */
class AudioPacketEpochTest {
    @Test fun successfulOwnedAdmissionPreventsWriteBeforeFenceCommits() {
        val gate = AudioPacketEpoch(); gate.resume(); val old = gate.capture()
        val entered = CountDownLatch(1); val nativeReturns = CountDownLatch(1)
        val closed = AtomicBoolean(false); val writes = AtomicInteger(0)
        val background = Thread {
            gate.closeAfterAdmission({ entered.countDown(); nativeReturns.await(); "" }) { closed.set(true) }
        }
        val reader = Thread { synchronized(gate) { if (gate.isCurrent(old)) writes.incrementAndGet() } }
        background.start()
        try {
            assertTrue(entered.await(1, TimeUnit.SECONDS)); reader.start()
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1)
            while (reader.state != Thread.State.BLOCKED && System.nanoTime() < deadline) Thread.sleep(1)
            assertEquals("A returned PCM packet must wait through native admission", Thread.State.BLOCKED, reader.state)
            assertFalse(closed.get()); assertEquals(0, writes.get())
            nativeReturns.countDown(); background.join(1000); reader.join(1000)
            assertFalse(background.isAlive); assertFalse(reader.isAlive)
            assertTrue(closed.get()); assertEquals(0, writes.get()); assertFalse(gate.admissionOpen())
        } finally {
            nativeReturns.countDown(); background.join(1000)
            if (reader.state != Thread.State.NEW) reader.join(1000)
        }
    }
    @Test fun rejectedOwnerOrFailedAdmissionLeavesNewOwnerSinkUntouched() {
        val gate = AudioPacketEpoch(); gate.resume(); val fresh = gate.capture()
        val closed = AtomicInteger(0)
        for (rejection in listOf("Display ownership retired", "Native pause admission failed")) {
            assertEquals(rejection, gate.closeAfterAdmission({ rejection }) { closed.incrementAndGet() })
            assertTrue(gate.isCurrent(fresh)); assertEquals(fresh, gate.currentGeneration())
        }
        assertEquals(0, closed.get())
    }
    @Test fun newOwnerResumeWaitsForAcceptedOldBackgroundFence() {
        val admission = WorkerLifecycleAdmission(); val gate = AudioPacketEpoch(); gate.resume()
        val old = gate.capture(); val admitted = CountDownLatch(1); val finishBackground = CountDownLatch(1)
        val userPaused = AtomicBoolean(false); val resumed = AtomicBoolean(false)
        val background = Thread { admission.run {
            gate.closeAfterAdmission({ admitted.countDown(); finishBackground.await(); "" }) { userPaused.set(true) }
        } }
        // The new native display may have been claimed already; its Resume and
        // focus publication must still follow the old accepted lifecycle action.
        val newOwner = Thread { admission.run { userPaused.set(false); gate.resume(); resumed.set(true) } }
        background.start()
        try {
            assertTrue(admitted.await(1, TimeUnit.SECONDS)); newOwner.start()
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1)
            while (newOwner.state != Thread.State.BLOCKED && System.nanoTime() < deadline) Thread.sleep(1)
            assertEquals(Thread.State.BLOCKED, newOwner.state); assertFalse(resumed.get())
            finishBackground.countDown(); background.join(1000); newOwner.join(1000)
            assertFalse(background.isAlive); assertFalse(newOwner.isAlive)
            assertTrue(resumed.get()); assertFalse(userPaused.get())
            assertTrue(gate.isCurrent(gate.capture())); assertFalse(gate.isCurrent(old))
        } finally {
            finishBackground.countDown(); background.join(1000)
            if (newOwner.state != Thread.State.NEW) newOwner.join(1000)
        }
    }
    @Test fun readCapturedBeforePauseCannotWriteAfterResume() {
        val gate = AudioPacketEpoch(); gate.resume()
        val captured = CountDownLatch(1); val readReturns = CountDownLatch(1)
        val written = AtomicInteger(0)
        val reader = Thread {
            val ticket = gate.capture() // Same production boundary before Native.audio.
            captured.countDown(); readReturns.await()
            synchronized(gate) { if (gate.isCurrent(ticket)) written.incrementAndGet() }
        }
        reader.start()
        try {
            assertTrue(captured.await(1, TimeUnit.SECONDS))
            gate.pause(); gate.resume() // Complete loss/gain while the read is in flight.
            readReturns.countDown(); reader.join(1000)
            assertFalse(reader.isAlive); assertEquals(0, written.get())
            val fresh = gate.capture()
            synchronized(gate) { if (gate.isCurrent(fresh)) written.incrementAndGet() }
            assertEquals(1, written.get())
        } finally { readReturns.countDown(); reader.join(1000) }
    }
    @Test fun closedReadCannotBecomeEligibleAfterResumeAndDuplicatePlayKeepsFreshRead() {
        val gate = AudioPacketEpoch()
        val closed = gate.capture(); assertEquals(0L, closed)
        gate.resume(); assertFalse(gate.isCurrent(closed))
        val fresh = gate.capture(); gate.resume()
        assertTrue(gate.isCurrent(fresh)) // Repeated play with held focus is not a transition.
        gate.pause(); val duringPause = gate.capture(); gate.resume()
        assertFalse(gate.isCurrent(fresh)); assertFalse(gate.isCurrent(duringPause))
        assertTrue(gate.isCurrent(gate.capture()))
    }
    @Test fun writingCriticalSectionCompletesBeforePausePublishesItsFence() {
        val gate = AudioPacketEpoch(); gate.resume(); val old = gate.capture()
        val writing = CountDownLatch(1); val releaseWrite = CountDownLatch(1)
        val pausePublished = AtomicBoolean(false); val writes = AtomicInteger(0)
        val writer = Thread {
            synchronized(gate) {
                if (gate.isCurrent(old)) { writing.countDown(); releaseWrite.await(); writes.incrementAndGet() }
            }
        }
        val pauser = Thread { gate.pause(); pausePublished.set(true) }
        writer.start()
        try {
            assertTrue(writing.await(1, TimeUnit.SECONDS)); pauser.start()
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1)
            while (pauser.state != Thread.State.BLOCKED && System.nanoTime() < deadline) Thread.sleep(1)
            assertEquals("Pause did not contend with the actual sink monitor", Thread.State.BLOCKED, pauser.state)
            assertFalse(pausePublished.get())
            releaseWrite.countDown(); writer.join(1000); pauser.join(1000)
            assertFalse(writer.isAlive); assertFalse(pauser.isAlive); assertTrue(pausePublished.get())
            assertEquals(1, writes.get()); gate.resume(); assertFalse(gate.isCurrent(old))
        } finally { releaseWrite.countDown(); writer.join(1000); if (pauser.state != Thread.State.NEW) pauser.join(1000) }
    }
}
