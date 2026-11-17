package org.t2dos.term

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

class SessionTest {
    private val HELLO = byteArrayOf(-1, -5, 1, -1, -5, 3, -1, -3, 31)       // IAC WILL ECHO, IAC WILL SGA, IAC DO NAWS

    private class Events : Session.Listener {
        val done = CountDownLatch(1); val calls = AtomicInteger(); @Volatile var reason = ""
        override fun changed() {}
        override fun closed(reason: String) { this.reason = reason; calls.incrementAndGet(); done.countDown() }
    }

    /** Reads from the client until `want` has arrived (or fails after 3 s); returns everything read. */
    private fun readUntil(s: Socket, want: ByteArray): ByteArray {
        val got = ByteArrayOutputStream(); val buf = ByteArray(4096)
        s.soTimeout = 3000
        while (!contains(got.toByteArray(), want)) { val n = s.getInputStream().read(buf); if (n < 0) break; got.write(buf, 0, n) }
        assertTrue("never got ${want.toList()} in ${got.toByteArray().toList()}", contains(got.toByteArray(), want))
        return got.toByteArray()
    }

    private fun contains(h: ByteArray, n: ByteArray): Boolean {
        if (n.isEmpty()) return true
        for (i in 0..h.size - n.size) if ((n.indices).all { h[i + it] == n[it] }) return true
        return false
    }

    private fun open(pass: String?, ev: Events): Pair<Session, Socket> {
        val ss = ServerSocket(0)
        val se = Session("127.0.0.1", ss.localPort, pass, 80, 24, ev)
        se.start()
        val s = ss.accept(); ss.close()
        return Pair(se, s)
    }

    @Test fun autoLoginOnce() {
        val ev = Events(); val (se, s) = open("secret", ev)
        val o = s.getOutputStream()
        o.write(HELLO); o.write("T2DOS ".toByteArray()); o.flush()
        Thread.sleep(300)
        o.write("password: ".toByteArray()); o.flush()
        val got = readUntil(s, "secret\r\u0000".toByteArray())
        assertTrue(contains(got, byteArrayOf(-1, -3, 1)))                   // IAC DO ECHO
        assertTrue(contains(got, byteArrayOf(-1, -6, 31, 0, 80, 0, 24, -1, -16)))   // the size
        o.write("\r\nwrong password\r\npassword: ".toByteArray()); o.flush()
        s.soTimeout = 1000
        val extra = try { s.getInputStream().read() } catch (e: SocketTimeoutException) { -2 }
        assertEquals("sent again after a wrong password", -2, extra)
        se.close(); s.close()
    }

    @Test fun closeReason() {
        val ev = Events(); val (_, s) = open(null, ev)
        s.getOutputStream().write("hi\r\n\r\n[logged out]\r\n".toByteArray()); s.getOutputStream().flush()
        s.close()
        assertTrue(ev.done.await(2, TimeUnit.SECONDS))
        assertEquals("[logged out]", ev.reason)
    }

    @Test fun refusedReason() {
        val ev = Events(); val (_, s) = open(null, ev)
        s.getOutputStream().write("try again later\r\n".toByteArray()); s.getOutputStream().flush()
        s.close()
        assertTrue(ev.done.await(2, TimeUnit.SECONDS))
        assertEquals("try again later", ev.reason)
    }

    @Test fun dropMidOutput() {
        val ev = Events(); val (se, s) = open(null, ev)
        val o = s.getOutputStream()
        for (i in 0 until 400) o.write("line $i of output text\r\n".toByteArray())
        o.flush(); Thread.sleep(300)
        s.setSoLinger(true, 0); s.close()                                 // a reset, not a clean close
        assertTrue(ev.done.await(2, TimeUnit.SECONDS))
        Thread.sleep(200)
        assertEquals(1, ev.calls.get())
        synchronized(se.screen) { assertEquals("line 399 of output text", se.screen.rowText(22)) }
    }

    @Test fun typedBytesEncoded() {
        val ev = Events(); val (se, s) = open(null, ev)
        se.send(Keys.key(Key.ENTER))
        val got = readUntil(s, byteArrayOf(13, 0))
        assertArrayEquals(byteArrayOf(13, 0), got)
        se.close(); s.close()
    }

    @Test fun silentBoardReported() {               // power cut or reset while idle: no bytes ever come back
        val ss = ServerSocket(0); val ev = Events()
        val t0 = System.currentTimeMillis()
        Session("127.0.0.1", ss.localPort, null, 80, 24, ev, idleMs = 300).start()
        val s = ss.accept(); ss.close()
        s.getOutputStream().write(HELLO); s.getOutputStream().flush()
        assertTrue(ev.done.await(4, TimeUnit.SECONDS))
        assertEquals("no answer from the board", ev.reason)
        val ms = System.currentTimeMillis() - t0
        assertTrue("gave up after $ms ms: three probes go unanswered first", ms >= 1100)
        s.close()
    }

    @Test fun lateAnswerKeepsIt() {                 // a WiFi hiccup: only the second probe is answered
        val ss = ServerSocket(0); val ev = Events()
        val se = Session("127.0.0.1", ss.localPort, null, 80, 24, ev, idleMs = 300)
        se.start()
        val s = ss.accept(); ss.close()
        Thread {
            val inp = s.getInputStream(); val o = s.getOutputStream(); var last = 0; var probes = 0
            try { while (true) { val c = inp.read(); if (c < 0) break
                if (last == 253 && c == 6) { probes++; if (probes % 3 == 2) { o.write(byteArrayOf(-1, -4, 6)); o.flush() } }
                last = c } } catch (_: Exception) {}
        }.start()
        assertTrue("closed: ${ev.reason}", !ev.done.await(2500, TimeUnit.MILLISECONDS))
        se.close(); s.close()
    }

    @Test fun answeringBoardStaysOpen() {           // telnetd answers IAC DO 6 with IAC WONT 6
        val ss = ServerSocket(0); val ev = Events()
        val se = Session("127.0.0.1", ss.localPort, null, 80, 24, ev, idleMs = 300)
        se.start()
        val s = ss.accept(); ss.close()
        Thread {
            val inp = s.getInputStream(); val o = s.getOutputStream(); var last = 0
            try { while (true) { val c = inp.read(); if (c < 0) break
                if (last == 253 && c == 6) { o.write(byteArrayOf(-1, -4, 6)); o.flush() }
                last = c } } catch (_: Exception) {}
        }.start()
        assertTrue("closed: ${ev.reason}", !ev.done.await(1500, TimeUnit.MILLISECONDS))
        se.close(); s.close()
    }

    @Test fun staleBracketNotReason() {
        val ev = Events(); val (_, s) = open(null, ev)
        s.getOutputStream().write("\r\n[exit 1, 12 ms]\r\n/> ".toByteArray()); s.getOutputStream().flush()
        Thread.sleep(200)
        s.setSoLinger(true, 0); s.close()
        assertTrue(ev.done.await(2, TimeUnit.SECONDS))
        assertTrue(ev.reason, ev.reason != "[exit 1, 12 ms]")
    }

    @Test fun reasonOnlyAtTheEnd() {
        assertEquals("[logged out]", Session.reasonFrom("x\r\n[logged out]\r\n"))
        assertEquals(null, Session.reasonFrom("\r\n[exit 1, 12 ms]\r\n/> "))
    }

    @Test fun cannotConnect() {
        val ss = ServerSocket(0); val port = ss.localPort; ss.close()
        val ev = Events()
        Session("127.0.0.1", port, null, 80, 24, ev).start()
        assertTrue(ev.done.await(6, TimeUnit.SECONDS))
        assertTrue(ev.reason, ev.reason.isNotEmpty())
    }
}
