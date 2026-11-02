package org.lighttouch.android

/** Selection facts only; native admission and guest consumption are separate. */
internal data class TouchTimestampSnapshot(
    val rawUptimeNs: Long = 0, val orderedUptimeNs: Long = 0,
    val selections: Long = 0, val adjustedSelections: Long = 0, val rejectedSelections: Long = 0
)

/** One engine worker owns this clock, including across pause/cancel/View recreation.
 * The caller serializes selection together with native admission. Android's public
 * MotionEvent.eventTime is millisecond-quantized; ties receive nanosecond ordinals.
 * Actual backwards source times remain errors. Never advance a full millisecond.
 */
internal class TouchTimestampOrder {
    @Volatile var snapshot = TouchTimestampSnapshot()
        private set

    fun select(rawUptimeNs: Long): Long {
        val previous = snapshot
        fun reject(message: String): Nothing {
            snapshot = previous.copy(rejectedSelections = previous.rejectedSelections + 1)
            throw IllegalArgumentException(message)
        }
        if (rawUptimeNs <= 0) reject("Touch timestamp must be positive")
        if (rawUptimeNs < previous.rawUptimeNs) reject("Touch source timestamp moved backwards")
        if (previous.orderedUptimeNs == Long.MAX_VALUE) reject("Touch timestamp ordering overflow")
        val ordered = maxOf(rawUptimeNs, previous.orderedUptimeNs + 1)
        if (ordered - rawUptimeNs >= 1_000_000) reject("Touch timestamp tie budget exhausted")
        snapshot = TouchTimestampSnapshot(rawUptimeNs, ordered, previous.selections + 1,
            previous.adjustedSelections + if (ordered != rawUptimeNs) 1 else 0, previous.rejectedSelections)
        return ordered
    }
}
