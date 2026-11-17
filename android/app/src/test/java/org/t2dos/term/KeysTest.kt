package org.t2dos.term

import org.junit.Assert.assertArrayEquals
import org.junit.Test

class KeysTest {
    private fun b(s: String) = s.toByteArray(Charsets.ISO_8859_1)
    private fun b(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    @Test fun specialKeys() {
        assertArrayEquals(b("\u001b[A"), Keys.key(Key.UP))
        assertArrayEquals(b("\u001b[B"), Keys.key(Key.DOWN))
        assertArrayEquals(b("\u001b[C"), Keys.key(Key.RIGHT))
        assertArrayEquals(b("\u001b[D"), Keys.key(Key.LEFT))
        assertArrayEquals(b("\u001b[H"), Keys.key(Key.HOME))
        assertArrayEquals(b("\u001b[F"), Keys.key(Key.END))
        assertArrayEquals(b("\u001b[5~"), Keys.key(Key.PGUP))
        assertArrayEquals(b("\u001b[6~"), Keys.key(Key.PGDN))
        assertArrayEquals(b("\u001b[3~"), Keys.key(Key.DEL))
        assertArrayEquals(b(27), Keys.key(Key.ESC))
        assertArrayEquals(b(9), Keys.key(Key.TAB))
        assertArrayEquals(b(13), Keys.key(Key.ENTER))
        assertArrayEquals(b(127), Keys.key(Key.BACKSPACE))
    }

    @Test fun text() {
        assertArrayEquals(b(1), Keys.text("a", ctrl = true))
        assertArrayEquals(b(3), Keys.text("C", ctrl = true))
        assertArrayEquals(b(26), Keys.text("z", ctrl = true))
        assertArrayEquals(b("1"), Keys.text("1", ctrl = true))
        assertArrayEquals(b(0xe5), Keys.text("å", false))
        assertArrayEquals(b(), Keys.text("€", false))
        assertArrayEquals(b(13), Keys.text("\n", false))
        assertArrayEquals(b("ls -l"), Keys.text("ls -l", false))
        assertArrayEquals(b(9), Keys.text("\t", false))
    }
}
