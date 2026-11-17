package org.t2dos.term

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream

class ScreenTest {
    private val replies = ByteArrayOutputStream()
    private fun scr(cols: Int = 20, rows: Int = 5) = Screen(cols, rows) { replies.write(it) }
    private fun Screen.f(s: String) = feed(s.toByteArray(Charsets.ISO_8859_1))
    private fun Screen.fb(vararg v: Int) = feed(ByteArray(v.size) { v[it].toByte() })
    private fun Screen.cur() = Pair(cursorRow, cursorCol)
    private val E = "\u001b"

    @Test fun printAndNewline() {
        val s = scr(); s.f("ab\r\ncd")
        assertEquals("ab", s.rowText(0)); assertEquals("cd", s.rowText(1)); assertEquals(Pair(1, 2), s.cur())
    }

    @Test fun backspaceTab() {
        val s = scr(); s.f("abc\b\bX")
        assertEquals("aXc", s.rowText(0))
        val t = scr(); t.f("a\tb")
        assertEquals('b', t.cell(0, 8).ch)
    }

    @Test fun clearHome() {
        val s = scr(); s.f("hello\r\nworld${E}[2J${E}[H")
        assertEquals("", s.rowText(0)); assertEquals("", s.rowText(1)); assertEquals(Pair(0, 0), s.cur())
        s.f("${E}[5;10H"); assertEquals(Pair(4, 9), s.cur())
        s.f("${E}[999;999H"); assertEquals(Pair(4, 19), s.cur())
        s.f("${E}[H"); assertEquals(Pair(0, 0), s.cur())
    }

    @Test fun eraseLine() {
        val s = scr(); s.f("abcdef\r\nxyz${E}[1;3H${E}[K")
        assertEquals("ab", s.rowText(0)); assertEquals("xyz", s.rowText(1))
    }

    @Test fun attributes() {
        val s = scr(); s.f("${E}[31;44mA${E}[7mB${E}[0m${E}[2mC${E}[mD")
        assertEquals(Cell('A', 1, 4, false, false), s.cell(0, 0))
        assertEquals(Cell('B', 1, 4, false, true), s.cell(0, 1))
        assertEquals(Cell('C', 7, 0, true, false), s.cell(0, 2))
        assertEquals(Cell('D', 7, 0, false, false), s.cell(0, 3))
    }

    @Test fun cursorVisibility() {
        val s = scr(); assertTrue(s.cursorVisible)
        s.f("${E}[?25l"); assertFalse(s.cursorVisible)
        s.f("${E}[?25h"); assertTrue(s.cursorVisible)
    }

    @Test fun altScreen() {
        val s = scr(); s.f("shell\r\n/> ")
        s.f("${E}[?1049h"); assertTrue(s.altScreen)
        assertEquals("", s.rowText(0))
        s.f("${E}[3;1Hedit")
        assertEquals("edit", s.rowText(2))
        s.f("${E}[2J${E}[H${E}[?1049l"); assertFalse(s.altScreen)
        assertEquals("shell", s.rowText(0)); assertEquals("/>", s.rowText(1)); assertEquals(Pair(1, 3), s.cur())
    }

    @Test fun cursorReport() {
        val s = scr(); s.f("${E}[5;10H${E}[6n")
        assertArrayEquals("${E}[5;10R".toByteArray(), replies.toByteArray())
    }

    @Test fun pendingWrap() {
        val s = scr(10, 5); s.f("0123456789\r\nx")
        assertEquals("0123456789", s.rowText(0)); assertEquals("x", s.rowText(1))
        val t = scr(10, 5); t.f("0123456789y")
        assertEquals("0123456789", t.rowText(0)); assertEquals("y", t.rowText(1))
        val u = scr(10, 5); u.f("0123456789${E}[K")              // erase at the pending-wrap spot keeps the line
        assertEquals("012345678", u.rowText(0).take(9))
    }

    @Test fun scrollAndHistory() {
        val s = scr(10, 3); s.f("l1\r\nl2\r\nl3\r\nl4\r\nl5")
        assertEquals("l3", s.rowText(0)); assertEquals("l5", s.rowText(2))
        assertEquals(2, s.scrollback)
        assertEquals("l2", s.historyRowText(0)); assertEquals("l1", s.historyRowText(1))
        s.f("${E}[?1049h\r\n\r\n\r\n\r\n")
        assertEquals(2, s.scrollback)                          // the alternate screen keeps no history
    }

    @Test fun historyLimit() {
        val s = scr(10, 3)
        for (i in 0 until 1200) s.f("$i\r\n")
        assertEquals(1000, s.scrollback)
    }

    @Test fun latin1() {
        val s = scr(); s.fb(0xe5, 0x85)
        assertEquals('å', s.cell(0, 0).ch); assertEquals('·', s.cell(0, 1).ch)
    }

    @Test fun unknownSequence() {
        val s = scr(); s.f("a${E}[12;34Xb${E}]0;title\u0007c")
        assertEquals("abc", s.rowText(0))
    }

    @Test fun splitSequence() {
        val s = scr(); s.f(E); s.f("[31m"); s.f("A")
        assertEquals(Cell('A', 1, 0, false, false), s.cell(0, 0))
    }

    @Test fun resizeKeepsTopLeft() {
        val s = scr(10, 3); s.f("abcdefghij\r\nklm")
        s.resize(5, 2)
        assertEquals(5, s.cols); assertEquals(2, s.rows)
        assertEquals("abcde", s.rowText(0)); assertEquals("klm", s.rowText(1))
        assertEquals(Pair(1, 3), s.cur())
        s.resize(30, 6)
        assertEquals("abcde", s.rowText(0))
    }

    @Test fun resizeKeepsCursorRowVisible() {      // the keyboard opens: the prompt must stay on screen
        val s = scr(10, 4); s.f("l1\r\nl2\r\nl3\r\n/> ")
        s.resize(10, 2)
        assertEquals("l3", s.rowText(0)); assertEquals("/>", s.rowText(1)); assertEquals(Pair(1, 3), s.cur())
        assertEquals("l2", s.historyRowText(0)); assertEquals("l1", s.historyRowText(1))
    }

    @Test fun resizeInAltKeepsShellLines() {       // rotate while edit is open: the shell's newest lines survive
        val s = scr(10, 6); s.f("l1\r\nl2\r\nl3\r\nl4\r\nl5\r\n/> ")
        s.f("${E}[?1049h"); s.resize(10, 3); s.f("${E}[?1049l")
        assertEquals("l4", s.rowText(0)); assertEquals("l5", s.rowText(1)); assertEquals("/>", s.rowText(2))
        assertEquals(Pair(2, 3), s.cur())
        assertEquals("l3", s.historyRowText(0)); assertEquals("l1", s.historyRowText(2))
    }

    @Test fun versionMoves() {
        val s = scr(); val v = s.version; s.f("x"); assertTrue(s.version > v)
    }

    @Test fun bigBurst() {
        val sb = StringBuilder()
        var i = 0
        while (sb.length < 200_000) { sb.append("${E}[3${i % 8}mline $i of text ${E}[0m\r\n"); if (i % 50 == 0) sb.append("${E}[2J${E}[H"); i++ }
        val all = sb.toString().toByteArray(Charsets.ISO_8859_1)
        val s = scr(80, 24)
        val t0 = System.nanoTime()
        var off = 0
        while (off < all.size) { val n = minOf(4096, all.size - off); s.feed(all, off, n); off += n }
        val ms = (System.nanoTime() - t0) / 1_000_000
        assertTrue("took $ms ms", ms < 1000)
    }
}
