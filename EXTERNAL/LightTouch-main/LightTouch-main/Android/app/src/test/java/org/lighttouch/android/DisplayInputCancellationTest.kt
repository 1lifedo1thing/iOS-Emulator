package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test

class DisplayInputCancellationTest {
    @Test fun onlyClosedBalancedCancellationReasonsAreExpected() {
        assertTrue(DisplayInputAdmission.isCancellation(DisplayInputAdmission.VIEWPORT_CANCELLED))
        assertTrue(DisplayInputAdmission.isCancellation(DisplayInputAdmission.PAUSED))
        assertTrue(DisplayInputAdmission.isCancellation(DisplayInputAdmission.GENERATION_RETIRED))
        assertTrue(DisplayInputAdmission.isCancellation(DisplayInputAdmission.NO_SESSION))
        assertTrue(DisplayInputAdmission.isCancellation(DisplayInputAdmission.NO_FRAME))
        for (failure in listOf("", "Touch left guest viewport; gesture cancelled",
                "Invalid touch coordinates or viewport; gesture cancelled",
                "${DisplayInputAdmission.VIEWPORT_CANCELLED}; input release failed: native rejected",
                "${DisplayInputAdmission.NO_FRAME}; input release failed: native rejected", "Native engine failed")) {
            assertFalse(failure, DisplayInputAdmission.isCancellation(failure))
        }
    }
    @Test fun workerHistoryBoundsReasonsAndKeepsPriorSnapshots() {
        val history = DisplayCancellationHistory()
        repeat(20) { index ->
            assertTrue(history.record(if (index % 2 == 0) DisplayInputAdmission.VIEWPORT_CANCELLED else DisplayInputAdmission.PAUSED))
        }
        assertEquals(20L, history.snapshot.count)
        assertEquals(DisplayCancellationHistory.LIMIT, history.snapshot.reasons.size)
        assertEquals(List(8) { if (it % 2 == 0) DisplayInputAdmission.VIEWPORT_CANCELLED else DisplayInputAdmission.PAUSED }, history.snapshot.reasons)
        val retained = history.snapshot
        assertFalse(history.record("input release failed: native rejected"))
        assertFalse(history.record("")) // A later admitted touch does not rewrite history.
        assertSame(retained, history.snapshot)
        assertTrue(history.record(DisplayInputAdmission.GENERATION_RETIRED))
        assertEquals(21L, history.snapshot.count)
        assertEquals(20L, retained.count) // Previously published snapshot is stable.
        assertEquals(DisplayInputAdmission.GENERATION_RETIRED, history.snapshot.reasons.last())
    }
}
