package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test

class ExperimentalCacheAdmissionTest {
    @Test fun cacheSelectionIsSerializedAndCannotConfigureConsumedWorker() {
        val admission = WorkerStartAdmission()
        var requested = false
        assertEquals("", admission.configureUnused("Experimental cache selection") { requested = true; "" })
        assertTrue(requested)
        assertEquals("", admission.configureUnused("Experimental cache selection") { requested = false; "" })
        assertFalse(requested)
        assertTrue(admission.tryConsume())
        assertEquals("Experimental cache selection requires a fresh unused worker",
            admission.configureUnused("Experimental cache selection") { error("Late selector reached native environment") })
    }
}
