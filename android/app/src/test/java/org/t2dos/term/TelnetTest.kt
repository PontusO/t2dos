package org.t2dos.term

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.ByteArrayOutputStream

class TelnetTest {
    private val IAC = 255; private val WILL = 251; private val WONT = 252; private val DO = 253; private val DONT = 254
    private val SB = 250; private val SE = 240

    private fun b(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    private class Rig {
        val sent = ByteArrayOutputStream()
        val t = Telnet { sent.write(it) }
        fun rx(vararg v: Int): ByteArray { val a = ByteArray(v.size) { v[it].toByte() }; return t.receive(a, 0, a.size) }
        fun take(): ByteArray { val r = sent.toByteArray(); sent.reset(); return r }
    }

    @Test fun hello() {
        val r = Rig()
        val data = r.rx(IAC, WILL, 1, IAC, WILL, 3, IAC, DO, 31)
        assertEquals(0, data.size)
        assertArrayEquals(b(IAC, DO, 1, IAC, DO, 3, IAC, WILL, 31, IAC, SB, 31, 0, 80, 0, 24, IAC, SE), r.take())
        assertEquals(true, r.t.echoByServer)
    }

    @Test fun refusesOthers() {
        val r = Rig()
        r.rx(IAC, DO, 24); assertArrayEquals(b(IAC, WONT, 24), r.take())
        r.rx(IAC, WILL, 24); assertArrayEquals(b(IAC, DONT, 24), r.take())
    }

    @Test fun iacDoubled() {
        val r = Rig()
        assertArrayEquals(b(255), r.rx(IAC, IAC))
        assertArrayEquals(b(255, 255, 13, 0), r.t.encode(b(255, 13)))
        assertArrayEquals(b('a'.code, 13, 0, 'b'.code), r.t.encode(b('a'.code, 13, 'b'.code)))
    }

    @Test fun splitAcrossReads() {
        val r = Rig()
        assertEquals(0, r.rx(IAC).size)
        assertEquals(0, r.rx(WILL, 1).size)
        assertArrayEquals(b(IAC, DO, 1), r.take())
        assertArrayEquals(b('x'.code), r.rx('x'.code, IAC))
        assertArrayEquals(b(255, 'y'.code), r.rx(IAC, 'y'.code))
    }

    @Test fun nawsOnResize() {
        val r = Rig()
        r.t.setSize(10, 5)
        assertEquals(0, r.take().size)                     // not agreed yet: nothing sent
        r.rx(IAC, DO, 31)
        assertArrayEquals(b(IAC, WILL, 31, IAC, SB, 31, 0, 10, 0, 5, IAC, SE), r.take())
        r.t.setSize(100, 30)
        assertArrayEquals(b(IAC, SB, 31, 0, 100, 0, 30, IAC, SE), r.take())
        r.t.setSize(255, 30)
        assertArrayEquals(b(IAC, SB, 31, 0, 255, 255, 0, 30, IAC, SE), r.take())
    }

    @Test fun subnegotiationSkipped() {
        val r = Rig()
        assertArrayEquals(b('h'.code, 'i'.code), r.rx(IAC, SB, 24, 1, IAC, SE, 'h'.code, 'i'.code))
    }

    @Test fun otherCommandsDropped() {
        val r = Rig()
        assertArrayEquals(b('a'.code, 'b'.code), r.rx('a'.code, IAC, 241, 'b'.code))   // IAC NOP
    }
}
