package org.lighttouch.android

import android.os.Bundle
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Synthetic evidence-policy checks; these do not establish guest rendering. */
@RunWith(AndroidJUnit4::class)
class GuestAngleFrameReadinessTest {
    private fun currentFrame(): Bundle = Bundle().apply {
        putString("state", "running")
        putBundle("observations", Bundle().apply {
            putBoolean("runtimeDiagnosticsAvailable", true)
            for ((key, value) in mapOf("engineReady" to 1L, "displaySleeping" to 0L,
                "backlightLevel" to 245L, "guestFrameWidth" to 320L, "guestFrameHeight" to 480L,
                "guestFrameSamplePixels" to 100L, "guestFrameNonzeroRgbSamples" to 2L,
                "guestFrameSequence" to 7L, "producerFrameSequence" to 7L,
                "guestFrameTimestampNs" to 100L, "producerFrameTimestampNs" to 100L,
                "hostMonotonicNs" to 10_000_000_000L, "framePublishedHostNs" to 9_000_000_000L)) putLong(key, value)
        })
    }

    @Test fun coherentStaticFrameNeedsNoNewProducerSerial() {
        val state = currentFrame()
        assertTrue(angleFrameReady(state, true))
        assertTrue(angleFrameReady(state, true))
    }

    @Test fun missingOrIncoherentCopiedFrameIsRejected() {
        for (key in listOf("guestFrameSequence", "guestFrameTimestampNs", "guestFrameWidth", "guestFrameHeight", "guestFrameSamplePixels")) {
            val state = currentFrame()
            state.getBundle("observations")!!.putLong(key, 0)
            assertFalse(key, angleFrameReady(state, true))
        }
        val state = currentFrame()
        state.getBundle("observations")!!.putLong("producerFrameSequence", 8)
        assertFalse(angleFrameReady(state, true))
    }

    @Test fun staleFutureAndUnlitFramesAreRejected() {
        for ((key, value) in mapOf("framePublishedHostNs" to 4_999_999_999L,
            "backlightLevel" to 0L, "displaySleeping" to 1L)) {
            val state = currentFrame()
            state.getBundle("observations")!!.putLong(key, value)
            assertFalse(key, angleFrameReady(state, true))
        }
        val future = currentFrame()
        future.getBundle("observations")!!.putLong("framePublishedHostNs", 10_000_000_001L)
        assertFalse(angleFrameReady(future, true))
    }

    @Test fun nonzeroRequirementIsExplicitAndDiagnosticsAreRequired() {
        val state = currentFrame()
        state.getBundle("observations")!!.putLong("guestFrameNonzeroRgbSamples", 0)
        assertFalse(angleFrameReady(state, true))
        assertTrue(angleFrameReady(state, false))
        state.getBundle("observations")!!.putBoolean("runtimeDiagnosticsAvailable", false)
        assertFalse(angleFrameReady(state, false))
    }

    @Test fun bothPhasesUseTheOriginalDeadline() {
        assertEquals(250L, anglePollSleep(1000, 600, 250))
        assertEquals(1L, anglePollSleep(1000, 999, 250))
        assertEquals(0L, anglePollSleep(1000, 1000, 250))
        assertEquals(0L, anglePollSleep(1000, 1100, 250))
        assertTrue(angleFrameReadyInTime(currentFrame(), true, 999, 1000))
        assertFalse(angleFrameReadyInTime(currentFrame(), true, 1000, 1000))
        assertFalse(angleFrameReadyInTime(currentFrame(), true, 1100, 1000))
    }

    @Test fun postCaptureCannotRelyOnEarlierReadiness() {
        val before = currentFrame()
        assertTrue(angleFrameReadyInTime(before, true, 900, 1000))
        val after = currentFrame()
        after.getBundle("observations")!!.putLong("producerFrameSequence", 8)
        assertFalse(angleFrameReadyInTime(after, true, 950, 1000))
        after.getBundle("observations")!!.putLong("guestFrameSequence", 8)
        assertTrue(angleFrameReadyInTime(after, true, 950, 1000))
        assertFalse(angleFrameReadyInTime(after, true, 1000, 1000))
    }
}
