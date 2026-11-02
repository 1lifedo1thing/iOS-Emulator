package org.lighttouch.android

import android.os.Bundle
import android.system.Os
import android.system.OsConstants
import java.io.File
import java.io.RandomAccessFile

/**
 * Engine-process counters for the performance overlay, read from procfs on each request:
 * the vCPU thread's run time (schedstat, ns), the core it last ran on and that core's clock,
 * and the whole worker's CPU time. The vCPU thread runs guest code and sleeps when the guest
 * idles (WFI), so its busy share is the guest's CPU load as the host sees it.
 */
internal class WorkerCpuSampler {
    private var vcpuTid = 0
    private val tickNs = 1_000_000_000L / maxOf(1L, Os.sysconf(OsConstants._SC_CLK_TCK))

    private fun findVcpu(): Int = File("/proc/self/task").listFiles()?.firstOrNull { task ->
        try { File(task, "comm").readText().trim() == "CPU 0/TCG" } catch (_: Exception) { false }
    }?.name?.toIntOrNull() ?: 0

    /** [vCPU run ns, last core, core kHz, worker CPU ns]; -1 for anything unreadable. */
    fun sample(): LongArray {
        val out = longArrayOf(-1, -1, -1, -1)
        if (vcpuTid == 0 || !File("/proc/self/task/$vcpuTid").exists()) vcpuTid = findVcpu()
        if (vcpuTid != 0) try {
            out[0] = File("/proc/self/task/$vcpuTid/schedstat").readText().trim().split(' ')[0].toLong()
            val fields = File("/proc/self/task/$vcpuTid/stat").readText().substringAfterLast(')').trim().split(' ')
            val core = fields[36].toLong()  // field 39, "processor"
            out[1] = core
            out[2] = File("/sys/devices/system/cpu/cpu$core/cpufreq/scaling_cur_freq").readText().trim().toLong()
        } catch (_: Exception) { vcpuTid = 0 }
        try {
            val fields = File("/proc/self/stat").readText().substringAfterLast(')').trim().split(' ')
            out[3] = (fields[11].toLong() + fields[12].toLong()) * tickNs  // utime + stime
        } catch (_: Exception) { }
        return out
    }
}

/**
 * Follows the engine's profile recording (events.tsv) while detailed timing is on, keeping
 * the last couple of seconds of its per-frame records so the overlay can show where frame
 * time goes: host GL time, vCPU run time and BQL waits. Records arrive through the engine's
 * buffered writer, so the newest ~0.5 s may not be visible yet.
 */
internal class ProfileTail {
    /** One "F" line: cumulative engine counters at a frame publication (see qemu-ios-ui.c). */
    data class Frame(val ns: Long, val run: Long, val outside: Long, val bqlOutside: Long, val bqlInside: Long,
                     val vcpuCpu: Long, val gl: Long, val glPresent: Long, val mainCpu: Long)
    private var file: File? = null
    private var offset = 0L
    private var partial = ""
    private val frames = ArrayDeque<Frame>()

    @Synchronized fun follow(events: File?) {
        if (events == file) return
        file = events; offset = 0L; partial = ""; frames.clear()
    }

    @Synchronized fun summary(): Bundle? {
        val source = file ?: return null
        try {
            RandomAccessFile(source, "r").use { input ->
                val length = input.length()
                if (length < offset) { offset = 0; partial = "" }
                // Never more than 1 MiB per poll; a fresh follow skips to the recent end.
                if (length - offset > 1 shl 20) { offset = length - (1 shl 20); partial = "" }
                val bytes = ByteArray((length - offset).toInt())
                input.seek(offset); input.readFully(bytes); offset = length
                val text = partial + String(bytes, Charsets.UTF_8)
                val lines = text.split('\n')
                partial = lines.last()
                for (line in lines.dropLast(1)) parse(line)?.let { frames.addLast(it); if (frames.size > 240) frames.removeFirst() }
            }
        } catch (_: Exception) { return null }
        val last = frames.lastOrNull() ?: return null
        val first = frames.lastOrNull { last.ns - it.ns >= 1_000_000_000L } ?: frames.first()
        val count = frames.count { it.ns > first.ns }
        if (count == 0 || last.ns <= first.ns) return null
        return Bundle().apply {
            putLong("windowNs", last.ns - first.ns); putInt("frames", count)
            putLong("glNs", last.gl - first.gl); putLong("glPresentNs", last.glPresent - first.glPresent)
            putLong("runNs", last.run - first.run); putLong("vcpuCpuNs", last.vcpuCpu - first.vcpuCpu)
            putLong("bqlNs", (last.bqlInside - first.bqlInside) + (last.bqlOutside - first.bqlOutside))
            putLong("mainCpuNs", last.mainCpu - first.mainCpu); putLong("lastFrameNs", last.ns)
        }
    }

    companion object {
        fun parse(line: String): Frame? {
            if (!line.startsWith("F ")) return null
            val v = line.split(' ')
            if (v.size < 12) return null
            return try {
                Frame(v[1].toLong(), v[3].toLong(), v[4].toLong(), v[5].toLong(), v[6].toLong(),
                    v[7].toLong(), v[9].toLong(), v[10].toLong(), v[11].toLong())
            } catch (_: NumberFormatException) { null }
        }
    }
}
