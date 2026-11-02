package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class DisplayOwnershipTest {
    @Test fun unfocusedStartedOverlapDoesNotStealAndFocusReturnNeedsNoNewStart() {
        val gate = ActiveDisplayOwner(); val a = Any(); val b = Any()
        gate.started(a); gate.resumed(a, false)
        val first = gate.focusChanged(a, true)
        gate.started(b); assertEquals(0L, gate.resumed(b, false))
        assertTrue(gate.canClaim(a, first))
        gate.focusChanged(a, false)
        gate.stopped(b, 0)
        // A stayed STARTED and RESUMED throughout this overlap.
        val returned = gate.focusChanged(a, true)
        assertEquals(first, returned); assertTrue(gate.canClaim(a, returned))
    }
    @Test fun focusedHandoffAndLegitimateReturnInvalidateStoredOldLease() {
        val gate = ActiveDisplayOwner(); val a = Any(); val b = Any(); val worker = Any()
        val lease = WorkerDisplayLease()
        gate.started(a); val first = gate.resumed(a, true); lease.bind(worker, 1, first)
        gate.focusChanged(a, false); gate.started(b); val latest = gate.resumed(b, true)
        assertFalse(gate.canClaim(a, first)); assertTrue(gate.canClaim(b, latest))
        // A's connection callback can only query eligibility, never promote A.
        assertFalse(gate.canClaim(a, first))
        gate.focusChanged(b, false)
        val returned = gate.focusChanged(a, true) // No A onStart or Surface destroy.
        assertTrue(returned > first); assertTrue(gate.canClaim(a, returned))
        assertEquals(0L, lease.forOwner(worker, returned)) // Must acquire a new native lease.
        lease.bind(worker, 3, returned)
        assertEquals(3L, lease.forOwner(worker, returned)); assertEquals(0L, lease.forOwner(worker, first))
    }
    @Test fun pausedOldFocusAndStopCannotPromoteOrRetireNewOwner() {
        val gate = ActiveDisplayOwner(); val a = Any(); val b = Any()
        gate.started(a); val old = gate.resumed(a, true); gate.paused(a)
        gate.started(b); val latest = gate.resumed(b, true)
        assertEquals(0L, gate.focusChanged(a, true))
        assertFalse(gate.stopped(a, old)); assertTrue(gate.canClaim(b, latest))
        assertEquals(0L, gate.focusChanged(a, true)) // Callback after stop is ignored.
    }
    @Test fun currentOwnerCanBackgroundAfterPauseWithoutAllowingNewClaims() {
        val gate = ActiveDisplayOwner(); val a = Any()
        gate.started(a); val ticket = gate.resumed(a, true); gate.paused(a)
        assertTrue(gate.owns(a, ticket)); assertFalse(gate.canClaim(a, ticket))
        assertTrue(gate.stopped(a, ticket)); assertFalse(gate.owns(a, ticket))
    }
    @Test fun duplicateFocusedCallbacksReuseTicketAndWorkerIdentityRemainsBound() {
        val gate = ActiveDisplayOwner(); val a = Any()
        gate.started(a); val ticket = gate.resumed(a, true)
        assertEquals(ticket, gate.focusChanged(a, true)); assertEquals(ticket, gate.resumed(a, true))
        val oldWorker = Any(); val newWorker = Any(); val lease = WorkerDisplayLease()
        lease.bind(oldWorker, 1, ticket)
        assertEquals(1L, lease.forOwner(oldWorker, ticket)); assertEquals(0L, lease.forOwner(newWorker, ticket))
        lease.bind(newWorker, 1, ticket)
        assertEquals(0L, lease.forWorker(oldWorker)); assertEquals(1L, lease.forOwner(newWorker, ticket))
        lease.clear(); assertEquals(0L, lease.forWorker(newWorker))
    }
    @Test fun audioFocusGainCannotInterleaveNativePauseAndUserFlagCommit() {
        val admission = WorkerLifecycleAdmission()
        val nativePaused = AtomicBoolean(false); val userPaused = AtomicBoolean(false)
        val entered = CountDownLatch(1); val commit = CountDownLatch(1); val gainAttempt = CountDownLatch(1)
        val gainFinished = CountDownLatch(1)
        val background = Thread {
            admission.run { nativePaused.set(true); entered.countDown(); check(commit.await(2, TimeUnit.SECONDS)); userPaused.set(true) }
        }
        val gain = Thread {
            gainAttempt.countDown()
            admission.run { if (!userPaused.get()) nativePaused.set(false) }
            gainFinished.countDown()
        }
        background.start()
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS)); gain.start(); assertTrue(gainAttempt.await(2, TimeUnit.SECONDS))
            val blockedDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1)
            while (gain.state != Thread.State.BLOCKED && System.nanoTime() < blockedDeadline && gainFinished.count > 0)
                Thread.sleep(1)
            assertEquals("Focus gain must block on the production admission monitor", Thread.State.BLOCKED, gain.state)
            assertEquals(1L, gainFinished.count)
        } finally { commit.countDown(); background.join(2000); if (gain.state != Thread.State.NEW) gain.join(2000) }
        assertFalse(background.isAlive); assertFalse(gain.isAlive)
        assertTrue(userPaused.get()); assertTrue(nativePaused.get())
        admission.run { userPaused.set(false); nativePaused.set(false) } // A later explicit Resume wins.
        assertFalse(userPaused.get()); assertFalse(nativePaused.get())
    }
}
