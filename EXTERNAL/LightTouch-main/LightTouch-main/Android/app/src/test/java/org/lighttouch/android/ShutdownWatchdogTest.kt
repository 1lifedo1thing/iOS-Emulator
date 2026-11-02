package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/** Tests the production coordinator with blocked functions; never loads/kills a native worker. */
class ShutdownWatchdogTest {
    private fun nowMillis(): Long = System.nanoTime() / 1_000_000
    private fun cleanStatus() = arrayOf("stopped", "", "", "", "", "platform", "0", "0", "1", "1", "0", "0", "0")
    @Test fun diagnosticPublicationFailureCannotPreventCleanTerminalDisposal() {
        for (failure in listOf(IllegalStateException("diagnostic clock failed"), OutOfMemoryError("diagnostic allocation failed"))) {
            val failNextDiagnostic = AtomicBoolean(false); val disposed = CountDownLatch(1)
            val terminal = AtomicReference<ShutdownReceipt>()
            val coordinator = ShutdownWatchdog(400, {
                if (failNextDiagnostic.compareAndSet(true, false)) throw failure
                nowMillis()
            }, { "" }, {}, {
                // The next publication records this return; core deadline/receipt clocks remain usable.
                failNextDiagnostic.set(true); cleanStatus()
            }, { terminal.set(it); disposed.countDown() })
            assertTrue(coordinator.request()); assertTrue(disposed.await(1, TimeUnit.SECONDS))
            assertFalse(failNextDiagnostic.get())
            assertEquals("native-terminal", terminal.get().reason); assertTrue(terminal.get().cleanConfirmed)
            assertEquals("native-terminal", coordinator.cachedProgress().disposalReason)
            assertNull("Failed diagnostic update cannot invent a cached poll", coordinator.cachedProgress().lastReturnedStatus)
        }
    }
    @Test fun cachedProgressRemainsReadableWhileEveryAdmissionStageIsActuallyBlocked() {
        for (blockedStage in listOf("shutdown-admission", "contact-cancellation", "terminal-poll", "terminal-receipt")) {
            val entered = CountDownLatch(1); val release = CountDownLatch(1); val returned = CountDownLatch(1)
            val disposed = CountDownLatch(1); val disposalCount = AtomicInteger()
            val polls = AtomicInteger(); val terminal = AtomicReference<ShutdownReceipt>()
            val reader = Executors.newSingleThreadExecutor { task -> Thread(task, "cached-progress-test-reader").apply { isDaemon = true } }
            fun block(stage: String) {
                if (stage == blockedStage) {
                    entered.countDown()
                    try { assertTrue(release.await(3, TimeUnit.SECONDS)) } finally { returned.countDown() }
                }
            }
            val coordinator = ShutdownWatchdog(400, ::nowMillis,
                requestNativeStop = { block("shutdown-admission"); "" },
                cancelNativeContacts = { block("contact-cancellation") },
                nativeStatus = {
                    if (blockedStage == "terminal-poll" && polls.incrementAndGet() == 1)
                        cleanStatus().also { it[0] = "running"; it[2] = "last returned poll"; it[9] = "0"; it[10] = "1" }
                    else { block("terminal-poll"); cleanStatus() }
                },
                dispose = { terminal.set(it); disposalCount.incrementAndGet(); disposed.countDown() },
                persistTerminal = { block("terminal-receipt") })
            fun read() = reader.submit<ShutdownDebugProgress> { coordinator.cachedProgress() }.get(100, TimeUnit.MILLISECONDS)
            try {
                val initial = read()
                assertFalse(initial.requested); assertNull(initial.acceptedAtMs); assertNull(initial.deadlineMs)
                assertNull(initial.lastReturnedStatus)
                assertTrue(coordinator.request()); assertTrue(entered.await(1, TimeUnit.SECONDS))
                val held = read(); assertEquals(blockedStage, held.phase)
                assertEquals(400L, held.deadlineMs!! - held.acceptedAtMs!!)
                assertTrue(held.phaseEnteredAtMs!! >= held.acceptedAtMs!!)
                val active = when (blockedStage) {
                    "shutdown-admission" -> held.stop
                    "contact-cancellation" -> held.cancel
                    "terminal-poll" -> held.status
                    else -> held.receipt
                }
                assertEquals(active.entered - 1, active.returned); assertEquals(0L, active.threw)
                if (blockedStage == "terminal-poll") {
                    assertEquals(2L, active.entered); assertEquals(1L, active.returned)
                    assertEquals("running", held.lastReturnedStatus!!.state)
                    assertEquals("last returned poll", held.lastReturnedStatus!!.error)
                    assertEquals(1L, held.lastReturnedStatus!!.pollNumber)
                    assertFalse(held.lastReturnedStatus!!.native.confirmed)
                    assertEquals(true, held.lastReturnedStatus!!.native.dirty)
                    assertTrue(held.lastReturnedStatus!!.returnedAtMs <= active.lastEnteredAtMs!!)
                } else if (blockedStage != "terminal-receipt") assertNull(held.lastReturnedStatus)
                var previous = held
                repeat(8) {
                    val next = read()
                    assertTrue(next.revision >= previous.revision)
                    for ((before, after) in listOf(previous.stop to next.stop, previous.cancel to next.cancel,
                        previous.status to next.status, previous.receipt to next.receipt)) {
                        assertTrue(after.entered >= before.entered); assertTrue(after.returned >= before.returned)
                        assertTrue(after.returned + after.threw <= after.entered)
                    }
                    previous = next
                }
                assertTrue("Independent deadline did not dispose blocked $blockedStage", disposed.await(2, TimeUnit.SECONDS))
                val atDeadline = read()
                assertEquals("deadline-unconfirmed", atDeadline.disposalReason)
                assertEquals(blockedStage, atDeadline.phase)
                assertFalse("Cached native flags cannot establish a persisted clean stop", terminal.get().cleanConfirmed)
                assertEquals(0L, active.threw)
                // The callback is still blocked even after deadline disposal.
                assertEquals(1L, release.count)
            } finally {
                release.countDown(); reader.shutdownNow()
                assertTrue(reader.awaitTermination(1, TimeUnit.SECONDS))
            }
            assertTrue(returned.await(1, TimeUnit.SECONDS)); Thread.sleep(20)
            assertEquals("Late callback disposed a second time", 1, disposalCount.get())
        }
    }

    @Test fun cachedStatusPreservesOnlyTheLastReturnedPollAndRecordsLaterException() {
        val secondPoll = CountDownLatch(1); val disposed = CountDownLatch(1); val polls = AtomicInteger()
        val original = arrayOf("running", "legacy detail", "real earlier error")
        val coordinator = ShutdownWatchdog(400, ::nowMillis, { "" }, {}, {
            if (polls.incrementAndGet() == 1) original
            else { secondPoll.countDown(); throw IllegalStateException("injected status failure") }
        }, { disposed.countDown() })
        assertTrue(coordinator.request()); assertTrue(secondPoll.await(1, TimeUnit.SECONDS))
        assertTrue(disposed.await(2, TimeUnit.SECONDS))
        original[0] = "stopped"; original[2] = "mutated external array"
        val snapshot = coordinator.cachedProgress()
        assertEquals("native-call-failed", snapshot.phase)
        assertEquals(2L, snapshot.status.entered); assertEquals(1L, snapshot.status.returned); assertEquals(1L, snapshot.status.threw)
        assertTrue(snapshot.status.lastException!!.contains("injected status failure"))
        assertEquals("running", snapshot.lastReturnedStatus!!.state)
        assertEquals("real earlier error", snapshot.lastReturnedStatus!!.error)
        assertFalse(snapshot.lastReturnedStatus!!.native.valid)
        assertNull(snapshot.lastReturnedStatus!!.native.dirty)
        assertEquals("deadline-unconfirmed", snapshot.disposalReason)
    }
    @Test fun deadlineSurvivesBlockedAdmissionCancellationAndStatus() {
        for (blockedStage in listOf("shutdown-admission", "contact-cancellation", "terminal-poll")) {
            val entered = CountDownLatch(1)
            val release = CountDownLatch(1)
            val returned = CountDownLatch(1)
            val disposed = CountDownLatch(1)
            val disposalCount = AtomicInteger(0)
            val observed = AtomicReference<ShutdownReceipt>()
            fun block(stage: String) {
                if (stage == blockedStage) {
                    entered.countDown()
                    try { assertTrue(release.await(3, TimeUnit.SECONDS)) } finally { returned.countDown() }
                }
            }
            val coordinator = ShutdownWatchdog(150, ::nowMillis,
                requestNativeStop = { block("shutdown-admission"); "" },
                cancelNativeContacts = { block("contact-cancellation") },
                nativeStatus = { block("terminal-poll"); arrayOf("stopped", "", "") },
                dispose = { observed.set(it); disposalCount.incrementAndGet(); disposed.countDown() })
            try {
                val before = nowMillis()
                assertTrue(coordinator.request())
                assertTrue("Request waited on blocked native path", nowMillis() - before < 100)
                assertTrue("Native path never reached $blockedStage", entered.await(1, TimeUnit.SECONDS))
                assertTrue("Independent disposal deadline did not fire", disposed.await(2, TimeUnit.SECONDS))
                val receipt = requireNotNull(observed.get())
                assertEquals("deadline-unconfirmed", receipt.reason)
                assertEquals(blockedStage, receipt.progress.stage)
                assertNull(receipt.terminalState)
                assertTrue(receipt.observedAtMs >= receipt.deadlineMs)
                assertTrue("Fallback was excessively delayed", receipt.observedAtMs - receipt.deadlineMs < 1000)
                assertFalse("Duplicate stop request admitted", coordinator.request())
            } finally { release.countDown() }
            assertTrue("Released native test callback did not return", returned.await(1, TimeUnit.SECONDS))
            Thread.sleep(200)
            assertEquals("Late native completion disposed twice", 1, disposalCount.get())
        }
    }

    @Test fun nativeTerminalWinsOnceBeforeGraceDeadline() {
        val disposed = CountDownLatch(1)
        val count = AtomicInteger(0)
        val receipt = AtomicReference<ShutdownReceipt>()
        val coordinator = ShutdownWatchdog(150, ::nowMillis, { "" }, {}, { arrayOf("stopped", "", "") }, {
            receipt.set(it); count.incrementAndGet(); disposed.countDown()
        })
        assertTrue(coordinator.request()); assertTrue(disposed.await(1, TimeUnit.SECONDS))
        assertEquals("native-terminal", receipt.get().reason)
        assertEquals("stopped", receipt.get().terminalState)
        assertTrue(receipt.get().observedAtMs < receipt.get().deadlineMs)
        Thread.sleep(300)
        assertEquals("Cancelled timer disposed twice", 1, count.get())
    }

    @Test fun blockedReceiptWriterCannotDelayDeadlineTermination() {
        val nativeEntered = CountDownLatch(1)
        val releaseNative = CountDownLatch(1)
        val nativeReturned = CountDownLatch(1)
        val writerEntered = CountDownLatch(1)
        val releaseWriter = CountDownLatch(1)
        val writerReturned = CountDownLatch(1)
        val terminated = CountDownLatch(1)
        val terminationCount = AtomicInteger(0)
        val observed = AtomicReference<ShutdownReceipt>()
        val coordinator = ShutdownWatchdog(150, ::nowMillis,
            requestNativeStop = {
                nativeEntered.countDown()
                try { assertTrue(releaseNative.await(3, TimeUnit.SECONDS)); "" } finally { nativeReturned.countDown() }
            }, cancelNativeContacts = {}, nativeStatus = { arrayOf("stopped", "", "") }, dispose = { receipt ->
                disposeWithoutWaitingForReceipt(writeReceipt = {
                    writerEntered.countDown()
                    try { assertTrue(releaseWriter.await(3, TimeUnit.SECONDS)) } finally { writerReturned.countDown() }
                }, terminate = { observed.set(receipt); terminationCount.incrementAndGet(); terminated.countDown() })
            })
        try {
            assertTrue(coordinator.request()); assertTrue(nativeEntered.await(1, TimeUnit.SECONDS))
            assertTrue("Termination waited for receipt IO", terminated.await(2, TimeUnit.SECONDS))
            assertTrue("Receipt writer was not actually exercised", writerEntered.await(1, TimeUnit.SECONDS))
            assertEquals("Writer released before termination proof", 1L, releaseWriter.count)
            assertEquals("deadline-unconfirmed", observed.get().reason)
            assertTrue(observed.get().observedAtMs >= observed.get().deadlineMs)
            assertEquals(1, terminationCount.get())
        } finally { releaseNative.countDown(); releaseWriter.countDown() }
        assertTrue(nativeReturned.await(1, TimeUnit.SECONDS)); assertTrue(writerReturned.await(1, TimeUnit.SECONDS))
        Thread.sleep(200)
        assertEquals("Late native/writer completion terminated twice", 1, terminationCount.get())
    }

    @Test fun terminalReceiptMustFinishBeforeDisposalWhileDeadlineCanKillBlockedWriter() {
        val entered = CountDownLatch(1); val release = CountDownLatch(1); val returned = CountDownLatch(1)
        val disposed = CountDownLatch(1); val count = AtomicInteger(0)
        val observed = AtomicReference<ShutdownReceipt>(); val cachedTerminal = AtomicReference<ShutdownReceipt>()
        val coordinator = ShutdownWatchdog(150, ::nowMillis, { "" }, {}, { cleanStatus() },
            dispose = { observed.set(it); count.incrementAndGet(); disposed.countDown() },
            persistTerminal = {
                cachedTerminal.set(it); entered.countDown()
                try { assertTrue(release.await(3, TimeUnit.SECONDS)) } finally { returned.countDown() }
            })
        try {
            assertTrue(coordinator.request()); assertTrue(entered.await(1, TimeUnit.SECONDS))
            assertTrue(cachedTerminal.get().cleanConfirmed)
            assertTrue("Deadline waited for terminal receipt IO", disposed.await(2, TimeUnit.SECONDS))
            assertEquals(1L, release.count)
            assertEquals("deadline-unconfirmed", observed.get().reason)
            assertEquals("terminal-receipt", observed.get().progress.stage)
            assertFalse("Deadline receipt claimed clean", observed.get().cleanConfirmed)
        } finally { release.countDown() }
        assertTrue(returned.await(1, TimeUnit.SECONDS)); Thread.sleep(200)
        assertEquals("Late terminal writer disposed twice", 1, count.get())
    }

    @Test fun persistedNativeFlagsPrecedeTerminalDisposal() {
        val written = AtomicReference<ShutdownReceipt>(); val disposed = CountDownLatch(1)
        val count = AtomicInteger(0)
        val coordinator = ShutdownWatchdog(500, ::nowMillis, { "" }, {}, { cleanStatus() },
            dispose = {
                assertSame("Disposed before immutable terminal receipt finished", written.get(), it)
                assertTrue(it.cleanConfirmed); count.incrementAndGet(); disposed.countDown()
            }, persistTerminal = { written.set(it) })
        assertTrue(coordinator.request()); assertTrue(disposed.await(1, TimeUnit.SECONDS))
        assertEquals("native-terminal", written.get().reason)
        Thread.sleep(600); assertEquals(1, count.get())
    }

    @Test fun legacyInvalidAndUncleanSnapshotsNeverConfirmShutdown() {
        val invalid = mutableListOf(arrayOf("stopped", "writable state clean; guest shutdown confirmed", ""))
        for ((index, value) in listOf(8 to "0", 8 to "invalid", 9 to "0", 10 to "1", 11 to "1", 12 to "1", 12 to "not-an-exit-code"))
            invalid += cleanStatus().also { it[index] = value }
        for (status in invalid) {
            val persisted = AtomicReference<ShutdownReceipt>(); val disposed = CountDownLatch(1)
            val coordinator = ShutdownWatchdog(500, ::nowMillis, { "" }, {}, { status },
                dispose = { assertFalse(it.cleanConfirmed); disposed.countDown() }, persistTerminal = { persisted.set(it) })
            assertTrue(coordinator.request()); assertTrue(disposed.await(1, TimeUnit.SECONDS))
            assertFalse(persisted.get().cleanConfirmed)
        }
        assertFalse(ShutdownNativeSnapshot.from(cleanStatus()).clean("failed"))
    }

    @Test fun failedTerminalReceiptIOKeepsDeadlineArmed() {
        val writeAttempt = CountDownLatch(1); val disposed = CountDownLatch(1)
        val observed = AtomicReference<ShutdownReceipt>(); val count = AtomicInteger(0)
        val coordinator = ShutdownWatchdog(150, ::nowMillis, { "" }, {}, { cleanStatus() },
            dispose = { observed.set(it); count.incrementAndGet(); disposed.countDown() },
            persistTerminal = { writeAttempt.countDown(); throw java.io.IOException("injected receipt IO failure") })
        assertTrue(coordinator.request()); assertTrue(writeAttempt.await(1, TimeUnit.SECONDS))
        assertTrue(disposed.await(2, TimeUnit.SECONDS))
        assertEquals("deadline-unconfirmed", observed.get().reason)
        assertEquals("terminal-receipt-failed", observed.get().progress.stage)
        assertTrue(observed.get().progress.error.contains("injected receipt IO failure"))
        assertFalse(observed.get().cleanConfirmed); assertEquals(1, count.get())
    }
}
