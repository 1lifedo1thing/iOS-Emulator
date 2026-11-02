package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test

class TouchTimestampOrderTest {
    private fun rejection(clock: TouchTimestampOrder, raw: Long): String {
        try { clock.select(raw); fail("Invalid source timestamp was accepted") }
        catch (failure: IllegalArgumentException) { return requireNotNull(failure.message) }
        error("Unreachable")
    }
    @Test fun equalMillisecondDownMovesAndUpKeepStrictNativeOrder() {
        val clock = TouchTimestampOrder()
        val millisecond = 123_000_000L
        val ordered = listOf("down", "move", "pointer-down", "move", "pointer-up", "up").map { clock.select(millisecond) }
        assertEquals((0L..5L).map { millisecond + it }, ordered)
        assertTrue(ordered.zipWithNext().all { (a, b) -> b > a })
        assertEquals(millisecond + 1_000_000, clock.select(millisecond + 1_000_000))
        assertEquals(5L, clock.snapshot.adjustedSelections)
        assertEquals(0L, clock.snapshot.rejectedSelections)
    }
    @Test fun backwardSourceRemainsRejectedWithoutPoisoningLaterUp() {
        val clock = TouchTimestampOrder()
        clock.select(20_000_000); clock.select(20_000_000)
        val previous = clock.snapshot
        assertTrue(rejection(clock, 19_000_000).contains("backwards"))
        assertEquals(previous.rawUptimeNs, clock.snapshot.rawUptimeNs)
        assertEquals(previous.orderedUptimeNs, clock.snapshot.orderedUptimeNs)
        assertEquals(previous.selections, clock.snapshot.selections)
        assertEquals(20_000_002L, clock.select(20_000_000))
        assertEquals(1L, clock.snapshot.rejectedSelections)
    }
    @Test fun finerSourceTimesAreOrderedWithoutRelaxingBackwardDetection() {
        val clock = TouchTimestampOrder()
        assertEquals(40_000_000L, clock.select(40_000_000))
        assertEquals(40_000_001L, clock.select(40_000_000))
        assertEquals(40_000_002L, clock.select(40_000_001))
        assertTrue(rejection(clock, 40_000_000).contains("backwards"))
        assertEquals(41_000_000L, clock.select(41_000_000))
    }
    @Test fun tiePaddingIsBoundedAndInvalidOverflowIsRejected() {
        val clock = TouchTimestampOrder()
        repeat(1_000_000) { assertEquals(100_000_000L + it, clock.select(100_000_000)) }
        assertTrue(rejection(clock, 100_000_000).contains("budget"))
        assertEquals(101_000_000L, clock.select(101_000_000))
        assertTrue(rejection(TouchTimestampOrder(), 0).contains("positive"))
        assertTrue(rejection(TouchTimestampOrder(), -1).contains("positive"))
        val finalClock = TouchTimestampOrder()
        assertEquals(Long.MAX_VALUE, finalClock.select(Long.MAX_VALUE))
        assertTrue(rejection(finalClock, Long.MAX_VALUE).contains("overflow"))
    }
}
