package org.lighttouch.android

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.provider.MediaStore
import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** A published guest frame: [intervalMs] since the previous one; presented is 0 if never posted. */
internal data class HitchFrame(val sequence: Long, val publishedNs: Long, val presentedNs: Long, val workNs: Long, val intervalMs: Float) {
    /** The first frame after a static screen: its interval is the idle time, not render time. */
    val idle get() = intervalMs >= IDLE_GAP_MS
}

/** Where one frame interval went, from detailed timing (deltas of the engine's F records, ms). */
internal data class FrameBreakdown(val endNs: Long, val intervalMs: Float, val runMs: Float, val outsideMs: Float,
                                   val bqlMs: Float, val vcpuCpuMs: Float, val glMs: Float, val glPresentMs: Float, val mainCpuMs: Float)

internal enum class EventKind(val label: String) { GL("GL call"), WAIT("Lock wait"), HOLD("Lock hold"), CALLBACK("Main loop") }
internal data class SlowEvent(val startNs: Long, val endNs: Long, val kind: EventKind, val label: String) {
    val ms get() = (endNs - startNs) / 1e6f
}

/** Everything the hitch inspector shows, loaded from one hitch snapshot directory. */
internal class HitchData(val directory: File, val frames: List<HitchFrame>, val breakdown: List<FrameBreakdown>,
                         val events: List<SlowEvent>, val snapshotNs: Long, val detailed: Boolean, val engineLog: String,
                         /** The iOS app in front when the inspector opened ("" when the guest could not say). */
                         val appName: String = "", val bundleId: String = "") {
    val startNs get() = frames.firstOrNull()?.publishedNs ?: snapshotNs
    val endNs get() = maxOf(frames.lastOrNull()?.publishedNs ?: snapshotNs, snapshotNs.coerceAtMost((frames.lastOrNull()?.publishedNs ?: 0) + 2_000_000_000L))

    fun framesIn(from: Long, to: Long) = frames.filter { it.publishedNs in from..to && !it.idle }
    fun breakdownIn(from: Long, to: Long) = breakdown.filter { it.endNs in from..to }
    fun eventsIn(from: Long, to: Long) = events.filter { it.endNs >= from && it.startNs <= to }
    /** The detailed record nearest a frame's publication (they come from the same engine event). */
    fun breakdownFor(frame: HitchFrame): FrameBreakdown? = breakdown.minByOrNull { kotlin.math.abs(it.endNs - frame.publishedNs) }
        ?.takeIf { kotlin.math.abs(it.endNs - frame.publishedNs) < 8_000_000L }

    companion object {
        /** Keep the last two minutes; older frames are outside what anyone pressed the button for. */
        private const val WINDOW_NS = 120_000_000_000L

        fun load(snapshot: android.os.Bundle): HitchData {
            val directory = File(requireNotNull(snapshot.getString("directory")))
            val snapshotNs = snapshot.getLong("snapshotNs")
            val raw = File(directory, "frames.tsv").readLines().filter { !it.startsWith("#") && it.isNotBlank() }.mapNotNull { line ->
                line.split(' ').takeIf { it.size >= 4 }?.map { it.toLong() }
            }.filter { snapshotNs - it[1] <= WINDOW_NS }
            val frames = raw.mapIndexed { i, v ->
                HitchFrame(v[0], v[1], v[2], v[3], if (i == 0) 0f else (v[1] - raw[i - 1][1]) / 1e6f)
            }.drop(1)
            val detailed = snapshot.getBoolean("detailed")
            val breakdown = ArrayList<FrameBreakdown>()
            val events = ArrayList<SlowEvent>()
            if (detailed) readEvents(File(directory, "events.tsv"), snapshotNs - WINDOW_NS, breakdown, events)
            return HitchData(directory, frames, breakdown, events, snapshotNs, detailed, snapshot.getString("engineLog") ?: "",
                snapshot.getString("appName") ?: "", snapshot.getString("bundleId") ?: "")
        }

        /** Parse the tail of a profile capture (at most 24 MiB) into per-frame deltas and slow events. */
        private fun readEvents(file: File, from: Long, breakdown: MutableList<FrameBreakdown>, events: MutableList<SlowEvent>) {
            if (!file.exists()) return
            val names = HashMap<Int, String>()
            val pendingGl = ArrayList<Triple<Long, Long, Int>>()
            var previous: ProfileTail.Frame? = null
            java.io.FileInputStream(file).use { stream ->
                val start = maxOf(0L, file.length() - (24L shl 20))
                stream.channel.position(start)
                val input = stream.reader(Charsets.UTF_8).buffered(1 shl 16)
                if (start > 0) input.readLine()  // Skip the partial first line.
                while (true) {
                    val line = input.readLine() ?: break
                    when (line.firstOrNull()) {
                        'F' -> ProfileTail.parse(line)?.let { f ->
                            val p = previous
                            if (p != null && f.ns >= from) breakdown.add(FrameBreakdown(f.ns, (f.ns - p.ns) / 1e6f,
                                (f.run - p.run) / 1e6f, (f.outside - p.outside) / 1e6f,
                                ((f.bqlInside - p.bqlInside) + (f.bqlOutside - p.bqlOutside)) / 1e6f,
                                (f.vcpuCpu - p.vcpuCpu) / 1e6f, (f.gl - p.gl) / 1e6f, (f.glPresent - p.glPresent) / 1e6f,
                                (f.mainCpu - p.mainCpu) / 1e6f))
                            previous = f
                        }
                        'G' -> line.split(' ').takeIf { it.size >= 7 }?.let { names[it[2].toIntOrNull() ?: -1] = it.drop(6).joinToString(" ") }
                        'S' -> line.split(' ').takeIf { it.size >= 4 }?.let { v ->
                            val s = v[1].toLongOrNull(); val e = v[2].toLongOrNull()
                            if (s != null && e != null && e >= from) pendingGl.add(Triple(s, e, v[3].toIntOrNull() ?: -1))
                        }
                        'W', 'H' -> line.split(' ').takeIf { it.size >= 5 }?.let { v ->
                            val s = v[1].toLongOrNull(); val e = v[2].toLongOrNull()
                            if (s != null && e != null && e >= from)
                                events.add(SlowEvent(s, e, if (line[0] == 'W') EventKind.WAIT else EventKind.HOLD, "${File(v[3]).name}:${v[4]}"))
                        }
                        'C' -> line.split(' ').takeIf { it.size >= 4 }?.let { v ->
                            val s = v[1].toLongOrNull(); val e = v[2].toLongOrNull()
                            if (s != null && e != null && e >= from) events.add(SlowEvent(s, e, EventKind.CALLBACK, v[3]))
                        }
                    }
                }
            }
            for ((s, e, slot) in pendingGl) events.add(SlowEvent(s, e, EventKind.GL, names[slot] ?: "GL call $slot"))
            events.sortBy { it.startNs }
        }
    }
}

/** Selection statistics as the inspector shows them. */
internal data class RangeStats(val frames: Int, val seconds: Float, val avgFps: Float, val avgMs: Float, val lowFps: Float,
                               val worstMs: Float, val hitches: Int, val buckets: IntArray) {
    companion object {
        val bucketLimits = floatArrayOf(16.7f, 20f, 25f, 33.4f, 50f, 100f)
        val bucketLabels = listOf("≤16.7 ms", "≤20 ms", "≤25 ms", "≤33 ms", "≤50 ms", "≤100 ms", ">100 ms")
        fun of(frames: List<HitchFrame>): RangeStats {
            val ms = frames.map { it.intervalMs }
            if (ms.isEmpty()) return RangeStats(0, 0f, 0f, 0f, 0f, 0f, 0, IntArray(bucketLabels.size))
            val total = ms.sum()
            val sorted = ms.sorted()
            val p99 = sorted[((sorted.size - 1) * 0.99f).toInt()]
            val buckets = IntArray(bucketLabels.size)
            for (v in ms) buckets[bucketLimits.indexOfFirst { v <= it }.let { if (it < 0) bucketLabels.size - 1 else it }]++
            return RangeStats(ms.size, total / 1000f, ms.size * 1000f / total, total / ms.size, 1000f / p99, sorted.last(),
                ms.count { it > 50f }, buckets)
        }
    }
}

/** Write the selected range as a zip in Downloads/LightTouch; returns its content URI. */
internal fun saveHitchReport(context: Context, data: HitchData, from: Long, to: Long): Uri {
    val stats = RangeStats.of(data.framesIn(from, to))
    val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(java.util.Date())
    // Named for the app it measured, so reports from different games tell apart at a glance.
    val app = data.appName.ifEmpty { data.bundleId.substringAfterLast('.') }
        .replace(Regex("[^A-Za-z0-9]+"), "-").trim('-').take(40)
    val name = "lighttouch-hitch-${if (app.isEmpty()) "" else "$app-"}$stamp.zip"
    val values = ContentValues().apply {
        put(MediaStore.Downloads.DISPLAY_NAME, name); put(MediaStore.Downloads.MIME_TYPE, "application/zip")
        put(MediaStore.Downloads.RELATIVE_PATH, "Download/LightTouch"); put(MediaStore.Downloads.IS_PENDING, 1)
    }
    val resolver = context.contentResolver
    val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: error("Cannot create the report in Downloads")
    try {
        resolver.openOutputStream(uri)?.use { raw -> ZipOutputStream(raw).use { zip ->
            fun entry(path: String, write: (java.io.OutputStream) -> Unit) { zip.putNextEntry(ZipEntry(path)); write(zip); zip.closeEntry() }
            entry("summary.txt") { out -> out.write(buildString {
                appendLine("LightTouch hitch report  ${java.util.Date()}")
                appendLine("Android ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}, LightTouch ${BuildConfig.VERSION_NAME}")
                appendLine("App in front: " + if (data.appName.isEmpty() && data.bundleId.isEmpty()) "unknown (the guest tools did not answer)"
                    else "${data.appName} (${data.bundleId})")
                appendLine("Selection: ${String.format(Locale.ROOT, "%.3f", (from - data.startNs) / 1e9)} s to ${String.format(Locale.ROOT, "%.3f", (to - data.startNs) / 1e9)} s of the timeline (CLOCK_MONOTONIC $from..$to)")
                appendLine(String.format(Locale.ROOT, "Frames %d over %.2f s: average %.1f fps (%.2f ms), 1%% low %.1f fps, worst %.1f ms, %d frames over 50 ms",
                    stats.frames, stats.seconds, stats.avgFps, stats.avgMs, stats.lowFps, stats.worstMs, stats.hitches))
                appendLine("Frame times:")
                RangeStats.bucketLabels.forEachIndexed { i, label -> appendLine("  ${label.padEnd(9)} ${stats.buckets[i]}") }
                appendLine(if (data.detailed) "Detailed timing: on (selection.tsv has the per-frame breakdown, events.tsv the raw records)"
                    else "Detailed timing: off (turn it on in Settings > Diagnostics for per-frame GL, CPU and lock timing)")
                val slow = data.eventsIn(from, to).sortedByDescending { it.ms }.take(20)
                if (slow.isNotEmpty()) { appendLine("Slowest events:"); slow.forEach { appendLine(String.format(Locale.ROOT, "  %7.2f ms  %-9s %s", it.ms, it.kind.label, it.label)) } }
            }.toByteArray()) }
            entry("selection.tsv") { out -> out.write(buildString {
                appendLine("# sequence published-ns presented-ns interval-ms presenter-ms run-ms outside-ms bql-ms vcpu-cpu-ms gl-ms gl-present-ms main-cpu-ms")
                for (f in data.framesIn(from, to)) {
                    val b = data.breakdownFor(f)
                    append("${f.sequence} ${f.publishedNs} ${f.presentedNs} ${String.format(Locale.ROOT, "%.3f %.3f", f.intervalMs, f.workNs / 1e6)}")
                    if (b != null) append(String.format(Locale.ROOT, " %.3f %.3f %.3f %.3f %.3f %.3f %.3f", b.runMs, b.outsideMs, b.bqlMs, b.vcpuCpuMs, b.glMs, b.glPresentMs, b.mainCpuMs))
                    appendLine()
                }
            }.toByteArray()) }
            File(data.directory, "events.tsv").takeIf { it.exists() }?.let { events ->
                entry("events.tsv") { out -> events.bufferedReader().useLines { lines -> lines.forEach { line ->
                    val t = line.split(' ').getOrNull(1)?.toLongOrNull()
                    if (line.startsWith("#") || (t != null && t in (from - 1_000_000_000L)..(to + 1_000_000_000L))) out.write((line + "\n").toByteArray())
                } } }
            }
            for (extra in listOf("metadata.json", "frames.tsv", "engine.log", "engine-start.log")) File(data.directory, extra).takeIf { it.exists() }?.let { f ->
                entry(extra) { out -> f.inputStream().use { it.copyTo(out) } }
            }
            // Without a capture, keep the end of this session's engine log instead.
            if (!File(data.directory, "engine.log").exists()) File(data.engineLog).takeIf { data.engineLog.isNotEmpty() && it.exists() }?.let { log ->
                entry("engine-tail.log") { out -> RandomAccessFile(log, "r").use { input ->
                    val start = maxOf(0L, input.length() - (2L shl 20)); input.seek(start)
                    val buffer = ByteArray(65536)
                    while (true) { val n = input.read(buffer); if (n <= 0) break; out.write(buffer, 0, n) }
                } }
            }
            entry("report.json") { out -> out.write(JSONObject().put("from", from).put("to", to).put("frames", stats.frames)
                .put("averageFps", stats.avgFps.toDouble()).put("lowFps", stats.lowFps.toDouble()).put("worstMs", stats.worstMs.toDouble())
                .put("hitches", stats.hitches).put("detailed", data.detailed).toString(2).toByteArray()) }
        } } ?: error("Cannot write the report")
        values.clear(); values.put(MediaStore.Downloads.IS_PENDING, 0)
        resolver.update(uri, values, null, null)
    } catch (e: Exception) { resolver.delete(uri, null, null); throw e }
    return uri
}
