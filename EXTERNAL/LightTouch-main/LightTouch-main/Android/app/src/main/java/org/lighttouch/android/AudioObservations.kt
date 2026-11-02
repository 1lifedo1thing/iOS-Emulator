package org.lighttouch.android

import android.os.Bundle
import kotlin.math.sqrt

/** Counts real production PCM and AudioTrack returns; never synthesizes samples. */
internal class AudioObservations {
    var pulledSamples = 0L
    var pulledNonzeroSamples = 0L
    private var pulledSquares = 0.0
    var writtenSamples = 0L
    var writtenNonzeroSamples = 0L
    private var writtenSquares = 0.0
    var writeCalls = 0L
    var shortWrites = 0L
    var writeFailures = 0L
    var nativePullFailures = 0L
    var discardedReadPackets = 0L
    var discardedReadSamples = 0L
    var underruns = 0L
    var playedFrames = 0L
    var focusLosses = 0L
    var focusGains = 0L
    var focusDenied = 0L
    var pauses = 0L
    var resumes = 0L
    var sampleRate = 0
    var channels = 0
    var lastWriteCode = 0
    var trackGenerations = 0L
    private var lastHead = 0L
    private var lastUnderruns = 0
    fun samples(data: ShortArray, offset: Int, count: Int, written: Boolean) {
        var nonzero = 0L; var squares = 0.0
        for (i in offset until offset + count) { val sample = data[i].toDouble(); if (sample != 0.0) nonzero++; squares += sample * sample }
        if (written) { writtenSamples += count; writtenNonzeroSamples += nonzero; writtenSquares += squares }
        else { pulledSamples += count; pulledNonzeroSamples += nonzero; pulledSquares += squares }
    }
    fun trackCreated(rate: Int, count: Int) { sampleRate = rate; channels = count; trackGenerations++; lastHead = 0; lastUnderruns = 0 }
    fun observeTrack(head: Int, underrunCount: Int) {
        val unsigned = head.toLong() and 0xffffffffL
        playedFrames += (unsigned - lastHead) and 0xffffffffL
        lastHead = unsigned
        underruns += (underrunCount - lastUnderruns).coerceAtLeast(0)
        lastUnderruns = underrunCount
    }
    fun flushed() { lastHead = 0 }
    fun snapshot() = Bundle().apply {
        putLong("pulledSamples", pulledSamples); putLong("pulledNonzeroSamples", pulledNonzeroSamples)
        putDouble("pulledRmsS16", if (pulledSamples == 0L) 0.0 else sqrt(pulledSquares / pulledSamples))
        putLong("writtenSamples", writtenSamples); putLong("writtenNonzeroSamples", writtenNonzeroSamples)
        putDouble("writtenRmsS16", if (writtenSamples == 0L) 0.0 else sqrt(writtenSquares / writtenSamples))
        putLong("writeCalls", writeCalls); putLong("shortWrites", shortWrites); putLong("writeFailures", writeFailures)
        putLong("nativePullFailures", nativePullFailures); putInt("lastWriteCode", lastWriteCode)
        putLong("discardedReadPackets", discardedReadPackets); putLong("discardedReadSamples", discardedReadSamples)
        putLong("underruns", underruns); putLong("playedFrames", playedFrames)
        putLong("focusLosses", focusLosses); putLong("focusGains", focusGains); putLong("focusDenied", focusDenied)
        putLong("pauses", pauses); putLong("resumes", resumes); putLong("trackGenerations", trackGenerations)
        putInt("sampleRate", sampleRate); putInt("channels", channels)
    }
}
