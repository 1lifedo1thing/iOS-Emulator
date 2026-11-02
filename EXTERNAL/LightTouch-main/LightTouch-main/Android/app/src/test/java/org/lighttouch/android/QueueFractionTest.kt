package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test

class QueueFractionTest {
    private fun item(id: Long, state: QueueState, overall: Float = 0f) = QueueItem(id, "k$id", "App $id", state, overall = overall)

    @Test fun anEmptyQueueHasNoProgress() = assertEquals(0f, queueFraction(emptyList()), 0f)

    @Test fun theRingFollowsAnItemsOverallProgress() {
        assertEquals(0.2f, queueFraction(listOf(item(1, QueueState.DOWNLOADING, QueueStages.download(.5f)))), 0.001f)
        assertEquals(0.4f, queueFraction(listOf(item(1, QueueState.READY, QueueStages.COPY))), 0.001f)
        assertEquals(0.55f, queueFraction(listOf(item(1, QueueState.INSTALLING, QueueStages.copy(.5f)))), 0.001f)
        assertEquals(1f, queueFraction(listOf(item(1, QueueState.DONE))), 0.001f)
    }

    @Test fun eachStageStartsWhereTheOneBeforeEnded() {
        assertEquals(QueueStages.download(1f), QueueStages.copy(0f), 0.0001f)
        assertEquals(QueueStages.copy(1f), QueueStages.unpack(0f), 0.0001f)
        assertEquals(QueueStages.unpack(1f), QueueStages.iosPercent(20), 0.0001f)
        assertEquals(1f, QueueStages.iosPercent(100), 0.0001f)
    }

    @Test fun finishedItemsCountWholeAmongWaitingOnes() {
        val items = listOf(item(1, QueueState.DONE), item(2, QueueState.FAILED), item(3, QueueState.PENDING), item(4, QueueState.PENDING))
        assertEquals(0.5f, queueFraction(items), 0.001f)
    }
}
