package org.t2dos.term

import java.io.ByteArrayOutputStream

/**
 * The telnet layer for the board's telnetd (kernel/telnetd.c): turns received bytes into data
 * and answers the options.  The board offers ECHO and SGA (accepted) and asks for NAWS (agreed,
 * then the window size is sent, and sent again on every change); anything else is refused.
 * Plain Kotlin, no Android types.  `reply` gets the bytes to send back.
 */
class Telnet(private val reply: (ByteArray) -> Unit) {
    private enum class St { DATA, IAC, OPT, SB, SB_IAC }

    private var st = St.DATA
    private var cmd = 0
    private var naws = false
    private var cols = 80
    private var rows = 24

    /** True once the board said it echoes (it always does). */
    var echoByServer = false
        private set

    /** The data bytes in buf[off, off + len); replies go to `reply`.  State carries across calls. */
    fun receive(buf: ByteArray, off: Int, len: Int): ByteArray {
        val out = ByteArrayOutputStream(len)
        for (i in off until off + len) {
            val c = buf[i].toInt() and 0xff
            when (st) {
                St.DATA -> if (c == IAC) st = St.IAC else out.write(c)
                St.IAC -> when (c) {
                    IAC -> { out.write(IAC); st = St.DATA }
                    in WILL..DONT -> { cmd = c; st = St.OPT }
                    SB -> st = St.SB
                    else -> st = St.DATA                  // NOP, GA, ...: nothing to do
                }
                St.OPT -> { option(cmd, c); st = St.DATA }
                St.SB -> if (c == IAC) st = St.SB_IAC
                St.SB_IAC -> st = if (c == SE) St.DATA else St.SB
            }
        }
        return out.toByteArray()
    }

    private fun option(cmd: Int, opt: Int) {
        when (cmd) {
            WILL -> if (opt == ECHO || opt == SGA) {
                if (opt == ECHO) echoByServer = true
                send(IAC, DO, opt)
            } else send(IAC, DONT, opt)
            DO -> if (opt == NAWS) {
                if (!naws) { naws = true; send(IAC, WILL, NAWS) }
                sendSize()
            } else send(IAC, WONT, opt)
            WONT -> if (opt == ECHO) echoByServer = false
            DONT -> if (opt == NAWS) naws = false
        }
    }

    /** Stores the window size; sends it when NAWS is agreed. */
    fun setSize(cols: Int, rows: Int) {
        this.cols = cols; this.rows = rows
        if (naws) sendSize()
    }

    private fun sendSize() {
        val o = ByteArrayOutputStream()
        o.write(IAC); o.write(SB); o.write(NAWS)
        for (v in intArrayOf(cols shr 8, cols, rows shr 8, rows)) {
            val b = v and 0xff
            o.write(b); if (b == IAC) o.write(IAC)
        }
        o.write(IAC); o.write(SE)
        reply(o.toByteArray())
    }

    /** Data to send: IAC doubled, CR followed by NUL. */
    fun encode(data: ByteArray): ByteArray {
        val o = ByteArrayOutputStream(data.size + 8)
        for (x in data) {
            val c = x.toInt() and 0xff
            o.write(c)
            if (c == IAC) o.write(IAC) else if (c == 13) o.write(0)
        }
        return o.toByteArray()
    }

    private fun send(vararg v: Int) = reply(ByteArray(v.size) { v[it].toByte() })

    companion object {
        const val IAC = 255; const val DONT = 254; const val DO = 253; const val WONT = 252; const val WILL = 251
        const val SB = 250; const val SE = 240
        const val ECHO = 1; const val SGA = 3; const val NAWS = 31
    }
}
