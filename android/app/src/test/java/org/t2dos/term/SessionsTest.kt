package org.t2dos.term

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class SessionsTest {
    private fun board(id: Long, port: Int) = Board(id, "b$id", "127.0.0.1", port, 14f, null)

    private class Seen : Session.Listener {
        val changed = CountDownLatch(1); val closed = CountDownLatch(1); @Volatile var reason = ""
        override fun changed() { changed.countDown() }
        override fun closed(reason: String) { this.reason = reason; closed.countDown() }
    }

    @Test fun attachSeesOutputAndCloseIsKept() {
        val ss = ServerSocket(0)
        val active = mutableListOf<Boolean>()
        Sessions.onActive = { synchronized(active) { active.add(it) } }
        val live = Sessions.connect(board(1, ss.localPort), null, 80, 24)
        val s = ss.accept(); ss.close()
        val a = Seen(); Sessions.attach(a)
        s.getOutputStream().write("hi\r\n".toByteArray()); s.getOutputStream().flush()
        assertTrue(a.changed.await(2, TimeUnit.SECONDS))
        Sessions.detach(a)
        s.getOutputStream().write("\r\n[logged out]\r\n".toByteArray()); s.getOutputStream().flush(); s.close()
        val t0 = System.currentTimeMillis()
        while (live.closedReason == null && System.currentTimeMillis() - t0 < 2000) Thread.sleep(20)
        assertEquals("[logged out]", live.closedReason)
        val b = Seen(); Sessions.attach(b)                      // the screen comes back after the close
        assertTrue(b.closed.await(1, TimeUnit.SECONDS)); assertEquals("[logged out]", b.reason)
        assertEquals(listOf(true, false), synchronized(active) { active.toList() })
        Sessions.detach(b)
    }

    @Test fun anotherBoardReplacesTheSession() {
        val s1 = ServerSocket(0); val s2 = ServerSocket(0)
        Sessions.onActive = {}
        val first = Sessions.connect(board(1, s1.localPort), null, 80, 24); val c1 = s1.accept()
        val second = Sessions.connect(board(2, s2.localPort), null, 80, 24); val c2 = s2.accept()
        assertNotSame(first, second)
        assertEquals(-1, c1.getInputStream().read())          // the first connection was closed
        assertEquals(second, Sessions.current)
        Sessions.disconnect()
        assertEquals(null, Sessions.current)
        c2.close(); s1.close(); s2.close()
    }
}
