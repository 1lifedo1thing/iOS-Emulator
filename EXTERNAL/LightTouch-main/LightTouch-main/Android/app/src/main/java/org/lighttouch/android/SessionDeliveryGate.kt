package org.lighttouch.android

/** Main-thread delivery gate for asynchronous IO belonging to a retired worker.
 * Advance before a bind/start/retirement or Activity destruction. No file read or
 * native operation is performed here; delayed IO posts back to the owner thread.
 */
internal class SessionDeliveryGate {
    private var generation = 0L
    fun advance(): Long = ++generation
    // Binder death and service disconnection can describe the same retirement.
    // A duplicate callback must preserve the first own-PID receipt token.
    fun retire(workerPresent: Boolean): Long? = if (workerPresent) advance() else null
    fun deliver(expected: Long, action: () -> Unit): Boolean {
        if (expected != generation) return false
        action()
        return true
    }
}
