package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test
import java.io.File

class InstallQueueTest {
    /** Steps wait in [tasks] until the test runs them, so each state in between can be looked at. */
    private class Harness {
        val tasks = mutableListOf<Pair<String, () -> Unit>>()
        val fetched = mutableListOf<String>()
        val installed = mutableListOf<String>()
        val discarded = mutableListOf<File>()
        val failures = mutableSetOf<String>()
        var busy = 0; var idle = 0
        var ignoreCancel = false
        val work = object : QueueWork<String> {
            override fun fetch(payload: String, report: QueueReport, cancelled: () -> Boolean): File {
                if (!ignoreCancel && cancelled()) throw StoreError("Download cancelled.")
                if ("fetch:$payload" in failures) throw StoreError("No connection")
                fetched += payload; return File("/tmp/$payload/App.ipa")
            }
            override fun install(payload: String, file: File, report: QueueReport, cancelled: () -> Boolean) {
                if ("commit:$payload" in failures) report.commit()
                if (cancelled()) throw StoreError("Cancelled")
                if ("install:$payload" in failures) throw StoreError("Install failed")
                installed += payload
            }
            override fun discard(file: File) { discarded += file }
        }
        val queue = InstallQueue(work) { name, body -> tasks += name to body }.also { it.onBusy = { busy++ }; it.onIdle = { idle++ } }
        fun run(name: String) { val index = tasks.indexOfFirst { it.first == name }; tasks.removeAt(index).second() }
        fun state(key: String) = queue.snapshot().first { it.key == key }.state
        fun add(vararg names: String) = names.forEach { queue.enqueue(it, it, it) }
    }

    @Test fun oneAppGoesThroughDownloadAndInstall() {
        val h = Harness()
        h.add("a")
        assertEquals(QueueState.DOWNLOADING, h.state("a"))
        assertEquals(1, h.busy)
        h.run("store-download")
        assertEquals(QueueState.INSTALLING, h.state("a"))   // the installer was free
        assertEquals(0, h.idle)
        h.run("store-install")
        assertEquals(QueueState.DONE, h.state("a"))
        assertEquals(listOf("a"), h.installed)
        assertEquals(listOf(File("/tmp/a/App.ipa")), h.discarded)
        assertEquals(1, h.idle); assertFalse(h.queue.active)
    }

    @Test fun copyingOntoTheDeviceCanBeCancelledUntilIOSInstalls() {
        val h = Harness()
        h.add("a")
        h.run("store-download")                              // a is installing: still copying onto the device
        val a = h.queue.snapshot().first { it.key == "a" }
        assertTrue(a.cancellable)
        h.queue.cancel(a.id)
        h.run("store-install")
        assertTrue(h.queue.snapshot().none { it.key == "a" })
        assertTrue("a" !in h.installed)
        assertEquals(listOf(File("/tmp/a/App.ipa")), h.discarded)
        assertFalse(h.queue.active)
    }

    @Test fun aCancelThatArrivesAfterIOSStartedInstallingIsTooLate() {
        val h = Harness()
        h.failures += "commit:a"
        h.add("a")
        h.run("store-download")
        h.queue.cancel(h.queue.snapshot().first { it.key == "a" }.id)   // before the step commits
        h.run("store-install")                                           // commits, then sees no cancel
        val a = h.queue.snapshot().first { it.key == "a" }
        assertEquals(QueueState.DONE, a.state)
        assertTrue(a.committed)
        assertFalse(a.cancellable)
    }

    @Test fun overallProgressNeverGoesBackAcrossStages() {
        val h = Harness()
        h.add("a")
        h.run("store-download")                              // downloaded: at least the copy stage's start
        assertEquals(QueueStages.COPY, h.queue.snapshot().first().overall, 0.0001f)
        h.run("store-install")
        assertEquals(1f, h.queue.snapshot().first().overall, 0.0001f)
    }

    @Test fun downloadOverlapsInstallButOnlyOneFileWaits() {
        val h = Harness()
        h.add("a", "b", "c")
        h.run("store-download")                              // a downloaded, installing; b starts downloading
        assertEquals(QueueState.INSTALLING, h.state("a"))
        assertEquals(QueueState.DOWNLOADING, h.state("b"))
        h.run("store-download")                              // b downloaded, waits for a
        assertEquals(QueueState.READY, h.state("b"))
        assertEquals(QueueState.PENDING, h.state("c"))       // no third file on disk yet
        h.run("store-install")                               // a done; b installs, c downloads
        assertEquals(QueueState.DONE, h.state("a"))
        assertEquals(QueueState.INSTALLING, h.state("b"))
        assertEquals(QueueState.DOWNLOADING, h.state("c"))
        h.run("store-download"); h.run("store-install"); h.run("store-install")
        assertEquals(listOf("a", "b", "c"), h.installed)
        assertEquals(1, h.busy); assertEquals(1, h.idle)
    }

    @Test fun installsRunOneAtATime() {
        val h = Harness()
        h.add("a", "b")
        h.run("store-download"); h.run("store-download")
        assertEquals(1, h.tasks.count { it.first == "store-install" })
    }

    @Test fun cancelRemovesPendingAndStopsDownloads() {
        val h = Harness()
        h.add("a", "b")
        h.queue.cancel(h.queue.snapshot().first { it.key == "b" }.id)
        assertEquals(listOf("a"), h.queue.snapshot().map { it.key })
        h.queue.cancel(h.queue.snapshot().first().id)          // a is downloading: flagged, then dropped when its step ends
        assertEquals("Cancelling", h.queue.snapshot().first().phase)
        h.run("store-download")
        assertTrue(h.queue.snapshot().isEmpty())
        assertEquals(1, h.idle)
        assertTrue(h.fetched.isEmpty())
    }

    @Test fun cancelledAfterFetchDiscardsTheFile() {
        val h = Harness()
        h.ignoreCancel = true   // a fetch that finished just as the cancel arrived still hands its file back
        h.add("a")
        h.queue.cancel(h.queue.snapshot().single().id)
        h.run("store-download")
        assertTrue(h.queue.snapshot().isEmpty())
        assertEquals(listOf(File("/tmp/a/App.ipa")), h.discarded)
        assertTrue(h.installed.isEmpty())
    }

    @Test fun failedItemsKeepTheirReasonAndRetryAtTheBack() {
        val h = Harness()
        h.failures += "fetch:a"
        h.add("a", "b")
        h.run("store-download")
        assertEquals(QueueState.FAILED, h.state("a"))
        assertEquals("No connection", h.queue.snapshot().first { it.key == "a" }.error)
        assertEquals(QueueState.DOWNLOADING, h.state("b"))   // the next app carries on
        assertEquals(0, h.idle)
        h.failures.clear()
        h.queue.retry(h.queue.snapshot().first { it.key == "a" }.id)
        assertEquals(listOf("b", "a"), h.queue.snapshot().map { it.key })
        assertNull(h.queue.snapshot().first { it.key == "a" }.error)
        while (h.tasks.isNotEmpty()) h.run(h.tasks.first().first)
        assertEquals(listOf("b", "a"), h.installed)
        assertEquals(1, h.idle)
    }

    @Test fun installFailureFreesTheInstallerAndDiscardsTheFile() {
        val h = Harness()
        h.failures += "install:a"
        h.add("a", "b")
        h.run("store-download"); h.run("store-download")
        h.run("store-install")
        assertEquals(QueueState.FAILED, h.state("a"))
        assertEquals("Install failed", h.queue.snapshot().first { it.key == "a" }.error)
        assertEquals(QueueState.INSTALLING, h.state("b"))
        assertTrue(File("/tmp/a/App.ipa") in h.discarded)
    }

    @Test fun anAppAlreadyInTheQueueIsNotAddedAgain() {
        val h = Harness()
        assertTrue(h.queue.enqueue("a", "a", "a"))
        assertFalse(h.queue.enqueue("a", "a", "a"))
        while (h.tasks.isNotEmpty()) h.run(h.tasks.first().first)
        assertEquals(QueueState.DONE, h.state("a"))
        assertTrue(h.queue.enqueue("a", "a", "a"))           // a finished one is replaced
        assertEquals(1, h.queue.snapshot().size)
        assertEquals(2, h.busy)
    }

    @Test fun finishedItemsStayUntilDismissed() {
        val h = Harness()
        h.add("a", "b")
        while (h.tasks.isNotEmpty()) h.run(h.tasks.first().first)
        assertEquals(2, h.queue.snapshot().size)
        h.queue.dismiss(h.queue.snapshot().first().id)
        assertEquals(listOf("b"), h.queue.snapshot().map { it.key })
        h.queue.clearFinished()
        assertTrue(h.queue.snapshot().isEmpty())
        assertEquals(1, h.idle)                              // dismissing doesn't announce idle again
    }

    @Test fun progressReportsReachTheItem() {
        val seen = mutableListOf<Pair<String, Float?>>()
        lateinit var queue: InstallQueue<String>
        val tasks = mutableListOf<() -> Unit>()
        val work = object : QueueWork<String> {
            override fun fetch(payload: String, report: QueueReport, cancelled: () -> Boolean): File {
                report.update("Downloading 1 MB of 4 MB", 0.25f); seen += queue.snapshot().single().let { it.phase to it.progress }
                report.rename("Renamed"); return File("/tmp/x/App.ipa")
            }
            override fun install(payload: String, file: File, report: QueueReport, cancelled: () -> Boolean) = Unit
            override fun discard(file: File) = Unit
        }
        queue = InstallQueue(work) { _, body -> tasks += body }
        queue.enqueue("k", "Original", "p")
        tasks.removeAt(0)()
        assertEquals(listOf("Downloading 1 MB of 4 MB" to 0.25f), seen)
        assertEquals("Renamed", queue.snapshot().single().name)
    }
}
