package org.lighttouch.android

import android.content.*
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.*
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class TcgSmokeTest {
    @Test fun armInstructionsExecuteInDedicatedAppUidWorker() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        var probe: ITcgProbe? = null
        val connected = CountDownLatch(1)
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) { probe = ITcgProbe.Stub.asInterface(binder); connected.countDown() }
            override fun onServiceDisconnected(name: ComponentName) = Unit
        }
        assertTrue(context.bindService(Intent(context, TcgProbeService::class.java), connection, Context.BIND_AUTO_CREATE))
        try {
            assertTrue("Probe binding timed out", connected.await(10, TimeUnit.SECONDS))
            val worker = requireNotNull(probe)
            val initial = worker.start()
            assertNotEquals("Probe must run outside UI process", Process.myPid(), initial.getInt("pid"))
            assertEquals("Dedicated worker must retain app UID", Process.myUid(), initial.getInt("uid"))
            val directory = File(requireNotNull(initial.getString("directory")))
            val socketPath = File(directory, "qmp.sock")
            val deadline = SystemClock.elapsedRealtime() + 30_000
            while (!socketPath.exists() && SystemClock.elapsedRealtime() < deadline) {
                assertEquals("Engine stopped before QMP: ${worker.status()}", Int.MIN_VALUE, worker.status().getInt("exitCode"))
                Thread.sleep(50)
            }
            assertTrue("QMP not ready; inspect ${File(directory, "engine.log")}", socketPath.exists())
            LocalSocket().use { socket ->
                socket.connect(LocalSocketAddress(socketPath.absolutePath, LocalSocketAddress.Namespace.FILESYSTEM))
                socket.soTimeout = 10_000
                val reader = socket.inputStream.bufferedReader()
                assertTrue(JSONObject(requireNotNull(reader.readLine())).has("QMP"))
                fun command(name: String, arguments: JSONObject? = null): JSONObject {
                    val request = JSONObject().put("execute", name)
                    arguments?.let { request.put("arguments", it) }
                    socket.outputStream.write((request.toString() + "\n").toByteArray())
                    while (true) {
                        val response = JSONObject(requireNotNull(reader.readLine()))
                        if (response.has("event")) continue
                        assertFalse(response.toString(), response.has("error"))
                        return response
                    }
                }
                command("qmp_capabilities")
                val word = File(directory, "word.bin")
                while (SystemClock.elapsedRealtime() < deadline) {
                    command("pmemsave", JSONObject().put("val", 0x40000100).put("size", 4).put("filename", word.absolutePath))
                    if (word.readBytes().contentEquals(byteArrayOf(42, 0, 0, 0))) break
                    Thread.sleep(50)
                }
                assertArrayEquals("Real ARM TCG must store word42", byteArrayOf(42, 0, 0, 0), word.readBytes())
                command("quit")
            }
            while (worker.status().getInt("exitCode") == Int.MIN_VALUE && SystemClock.elapsedRealtime() < deadline + 10_000) Thread.sleep(50)
            val terminal = worker.status()
            assertEquals(0, terminal.getInt("exitCode"))
            File(directory, "result.json").writeText(JSONObject().put("cpuWord", 42).put("workerPid", initial.getInt("pid"))
                .put("appUid", Process.myUid()).put("hostAbi", Build.SUPPORTED_ABIS[0]).put("engineExit", 0)
                .put("iosBootProven", false).put("graphicsProven", false).toString(2))
        } finally { probe?.finish(); context.unbindService(connection) }
    }
}
