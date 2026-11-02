package org.lighttouch.android

import android.content.*
import android.os.*
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Instrumentation calls the same non-exported Binder services as the activity. */
internal class AcceptanceBinding<T>(private val context: Context, intent: Intent, adapt: (IBinder) -> T) : AutoCloseable {
    private val connected = CountDownLatch(1)
    private var value: T? = null
    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) { value = adapt(binder); connected.countDown() }
        override fun onServiceDisconnected(name: ComponentName) = Unit
    }
    init { check(context.bindService(intent, connection, Context.BIND_AUTO_CREATE)) }
    fun get(): T { check(connected.await(10, TimeUnit.SECONDS)) { "Service connection timed out" }; return requireNotNull(value) }
    override fun close() { context.unbindService(connection) }
}
internal fun acceptanceSha256(file: File): String {
    val digest = MessageDigest.getInstance("SHA-256")
    file.inputStream().use { input -> val buffer = ByteArray(1024 * 1024); while (true) { val n = input.read(buffer); if (n < 0) break; digest.update(buffer, 0, n) } }
    return digest.digest().joinToString("") { "%02x".format(it) }
}
