package org.t2dos.term

import java.io.ByteArrayOutputStream

enum class Key { UP, DOWN, LEFT, RIGHT, HOME, END, PGUP, PGDN, DEL, ESC, TAB, ENTER, BACKSPACE }

/** The bytes the board's programs expect for each key (userland/edit.c getkey, userland/sh.c). */
object Keys {
    fun key(k: Key): ByteArray = when (k) {
        Key.UP -> esc("[A"); Key.DOWN -> esc("[B"); Key.RIGHT -> esc("[C"); Key.LEFT -> esc("[D")
        Key.HOME -> esc("[H"); Key.END -> esc("[F"); Key.PGUP -> esc("[5~"); Key.PGDN -> esc("[6~")
        Key.DEL -> esc("[3~")
        Key.ESC -> byteArrayOf(27); Key.TAB -> byteArrayOf(9); Key.ENTER -> byteArrayOf(13)
        Key.BACKSPACE -> byteArrayOf(127)
    }

    /**
     * Typed or pasted text: one byte per character (Latin-1).  With ctrl, letters become 1..26.
     * Newline becomes CR; characters outside ASCII and Latin-1 (160-255) are dropped.
     */
    fun text(s: String, ctrl: Boolean): ByteArray {
        val o = ByteArrayOutputStream(s.length)
        for (ch in s) {
            val c = ch.code
            when {
                ctrl && ch in 'a'..'z' -> o.write(c - 'a'.code + 1)
                ctrl && ch in 'A'..'Z' -> o.write(c - 'A'.code + 1)
                ch == '\n' || ch == '\r' -> o.write(13)
                ch == '\t' -> o.write(9)
                c in 32..126 || c in 160..255 -> o.write(c)
            }
        }
        return o.toByteArray()
    }

    private fun esc(s: String) = byteArrayOf(27) + s.toByteArray(Charsets.ISO_8859_1)
}
