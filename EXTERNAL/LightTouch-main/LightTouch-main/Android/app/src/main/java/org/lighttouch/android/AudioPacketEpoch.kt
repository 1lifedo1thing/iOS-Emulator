package org.lighttouch.android

/** Also the sink monitor: final ticket check and AudioTrack write hold this lock. */
internal class AudioPacketEpoch {
    private var generation = 1L
    private var open = false
    @Synchronized fun capture(): Long = if (open) generation else 0L
    @Synchronized fun isCurrent(ticket: Long) = open && ticket > 0L && ticket == generation
    @Synchronized fun pause() { advance(); open = false }
    /** Owner rejection changes nothing; successful admission cannot race a sink write. */
    @Synchronized fun closeAfterAdmission(admit: () -> String, closed: () -> Unit): String {
        val result = admit()
        if (result.isEmpty()) { pause(); closed() }
        return result
    }
    @Synchronized fun resume() {
        if (!open) { advance(); open = generation > 0L }
    }
    @Synchronized fun currentGeneration() = generation
    @Synchronized fun admissionOpen() = open
    private fun advance() {
        generation = if (generation in 1 until Long.MAX_VALUE) generation + 1 else 0L
    }
}
