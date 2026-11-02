package org.lighttouch.android

import org.junit.Assert.*
import org.junit.Test

class StoreVersionsTest {
    @Test fun readsUpToThreeNumbers() {
        assertEquals(listOf(1, 45, 0), StoreVersions.parse("1.45"))
        assertEquals(listOf(1, 0, 0), StoreVersions.parse("1.00"))
        assertEquals(listOf(4, 0, 5), StoreVersions.parse("4.0.5"))
        assertNull(StoreVersions.parse("1.2.3.4"))
        assertNull(StoreVersions.parse("v2"))
        assertNull(StoreVersions.parse(null))
    }

    @Test fun newestIsByNumberNotByText() {
        assertEquals("1.10", StoreVersions.newest(listOf("1.9", "1.10", "1.2")))
        assertEquals("2.0", StoreVersions.newest(listOf("Unknown", "1.0", "2.0")))
        assertNull(StoreVersions.newest(emptyList()))
        assertNull(StoreVersions.newest(listOf("Unknown")))
    }

    @Test fun anUpdateIsOnlyClaimedForAVersionThatIsReallyNewer() {
        assertEquals("1.27", StoreVersions.updateTo("1.26", "1.27"))
        assertEquals("2.0.1", StoreVersions.updateTo("2.0", "2.0.1"))
        assertNull(StoreVersions.updateTo("1.27", "1.27"))
        assertNull(StoreVersions.updateTo("1.28", "1.27"))
        assertNull(StoreVersions.updateTo("1.0", "1.0.0"))
        // Unreadable on either side: say nothing rather than guess.
        assertNull(StoreVersions.updateTo(null, "1.27"))
        assertNull(StoreVersions.updateTo("1.2.3.4", "1.27"))
        assertNull(StoreVersions.updateTo("1.0", "Unknown"))
    }
}
