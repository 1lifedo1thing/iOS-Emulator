package org.lighttouch.android

import android.app.Service
import android.content.Intent
import android.os.*
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/** Debug-only legal CPU/JIT probe. No private firmware and no configurable guest argv. */
class TcgProbeService : Service() {
    private val consumed = AtomicBoolean(false)
    @Volatile private var exitCode = Int.MIN_VALUE
    @Volatile private var failure = ""
    private lateinit var directory: File
    private val binder = object : ITcgProbe.Stub() {
        override fun start(): Bundle {
            check(consumed.compareAndSet(false, true)) { "A fresh probe worker is required" }
            directory = File(filesDir, "tcg-${java.lang.Long.toHexString(SystemClock.uptimeMillis())}")
            check(directory.mkdir())
            // MOV r0,#42; MOV r1,#0x40000000; STR r0,[r1,#0x100]; B .
            val code = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
            for (word in listOf(0xe3a0002a.toInt(), 0xe3a01101.toInt(), 0xe5810100.toInt(), 0xeafffffe.toInt())) code.putInt(word)
            File(directory, "guest.bin").writeBytes(code.array())
            Thread({
                try { exitCode = TcgProbeNative.run(directory.absolutePath) }
                catch (e: Throwable) { failure = e.toString(); exitCode = -1 }
            }, "tcg-probe").start()
            return status()
        }
        override fun status(): Bundle = Bundle().apply {
            putString("directory", if (::directory.isInitialized) directory.absolutePath else "")
            putInt("pid", Process.myPid()); putInt("uid", Process.myUid()); putInt("exitCode", exitCode); putString("error", failure)
        }
        override fun finish() { Handler(mainLooper).postDelayed({ stopSelf(); Process.killProcess(Process.myPid()) }, 100) }
    }
    override fun onBind(intent: Intent): IBinder = binder
}
internal object TcgProbeNative {
    init { System.loadLibrary("lighttouch_probe") }
    external fun run(directory: String): Int
}
