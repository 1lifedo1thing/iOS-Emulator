package org.lighttouch.android

/** Serialize pre-start configuration with irreversible worker consumption. */
internal class WorkerStartAdmission {
    @Volatile private var consumed = false
    fun isConsumed(): Boolean = consumed
    @Synchronized fun tryConsume(): Boolean {
        if (consumed) return false
        consumed = true
        return true
    }
    @Synchronized fun configureUnused(feature: String = "vCPU accounting", configure: () -> String): String {
        if (consumed) return "$feature requires a fresh unused worker"
        return configure()
    }
}
