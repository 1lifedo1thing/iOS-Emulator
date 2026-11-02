package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test

class WorkerSurfaceIdentityTest {
    private class EqualButDistinct {
        override fun equals(other: Any?) = other is EqualButDistinct
        override fun hashCode() = 1
    }
    @Test fun workerIncarnationChangesSurfaceKeyEvenWhenObjectsCompareEqual() {
        val a = EqualButDistinct(); val b = EqualButDistinct()
        val keyA = WorkerSurfaceIdentity(a)
        assertEquals(keyA, WorkerSurfaceIdentity(a)) // Status recomposition/rebind same endpoint.
        assertEquals(keyA.hashCode(), WorkerSurfaceIdentity(a).hashCode())
        assertNotEquals(keyA, WorkerSurfaceIdentity(b))
        assertFalse(keyA.matches(b)) // Late A callback cannot attach/cancel new B.
        assertTrue(keyA.matches(a))
        assertNotEquals(keyA, WorkerSurfaceIdentity(null))
        assertEquals(WorkerSurfaceIdentity(null), WorkerSurfaceIdentity(null))
    }
    @Test fun oldViewNeverMatchesReplacementButLegitimateReturnCanUseItsOwnEndpoint() {
        val a = Any(); val b = Any(); var current: Any? = a
        val capturedA = WorkerSurfaceIdentity(a)
        assertTrue(capturedA.matches(current))
        current = null
        assertFalse(capturedA.matches(current))
        current = b
        assertFalse(capturedA.matches(current))
        assertTrue(WorkerSurfaceIdentity(b).matches(current))
    }
}
