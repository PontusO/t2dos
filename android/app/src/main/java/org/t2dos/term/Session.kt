package org.t2dos.term

import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executors

/**
 * One connection to a board's telnetd.  A reader thread feeds the screen (callers lock `screen`
 * with synchronized(screen) to read it); writes go out on their own thread so the UI never
 * waits for the network.  With a password, it is typed once when the board asks for it, never
 * again on that connection (so a wrong one cannot run into the board's 3-try lock).
 */
class Session(
    private val host: String, private val port: Int, private val password: String?,
    cols: Int, rows: Int, private val listener: Listener,
) {
    interface Listener {
        fun changed()                       // the screen changed (called on the session thread)
        fun closed(reason: String)          // the connection ended, once
    }

    val screen = Screen(cols, rows) { send0(it) }
    private val telnet = Telnet { send0(it) }
    private val writer = Executors.newSingleThreadExecutor()
    @Volatile private var sock: Socket? = null
    @Volatile private var out: OutputStream? = null
    @Volatile private var ended = false
    private var triedPassword = false
    private val tail = StringBuilder()           // the last bytes received, for the close reason

    init { telnet.setSize(cols, rows) }

    fun start() { Thread({ run() }, "t2dos-session").apply { isDaemon = true }.start() }

    private fun run() {
        var error: String? = null
        try {
            val s = Socket()
            s.connect(InetSocketAddress(host, port), 5000)
            s.tcpNoDelay = true
            sock = s; out = s.getOutputStream()
            val inp = s.getInputStream()
            val buf = ByteArray(4096)
            while (!ended) {
                val n = inp.read(buf)
                if (n < 0) break
                val data = synchronized(telnet) { telnet.receive(buf, 0, n) }
                if (data.isEmpty()) continue
                synchronized(screen) { screen.feed(data) }
                remember(data)
                login()
                listener.changed()
            }
        } catch (e: IOException) {
            error = e.message ?: e.javaClass.simpleName
        }
        finish(error)
    }

    private fun remember(data: ByteArray) {
        tail.append(String(data, Charsets.ISO_8859_1))
        if (tail.length > 400) tail.delete(0, tail.length - 400)
    }

    private fun login() {
        if (password == null || triedPassword) return
        val asks = synchronized(screen) {
            val r = screen.cursorRow
            val sb = StringBuilder()
            for (c in 0 until screen.cursorCol) sb.append(screen.cell(r, c).ch)
            sb.endsWith("password: ")
        }
        if (asks) { triedPassword = true; send(Keys.text(password, false) + byteArrayOf(13)) }
    }

    private fun finish(error: String?) {
        val reason = reasonFrom(tail.toString()) ?: error ?: "connection closed"
        val first = synchronized(this) { val f = !closedSent; closedSent = true; f }
        ended = true
        try { sock?.close() } catch (_: IOException) {}
        writer.shutdown()
        if (first) listener.closed(reason)
    }
    private var closedSent = false

    /** Sends typed bytes (encoded for telnet). */
    fun send(bytes: ByteArray) {
        if (bytes.isEmpty()) return
        val enc = synchronized(telnet) { telnet.encode(bytes) }
        send0(enc)
    }

    private fun send0(raw: ByteArray) {
        if (ended) return
        try {
            writer.execute {
                try { out?.let { it.write(raw); it.flush() } } catch (_: IOException) { try { sock?.close() } catch (_: IOException) {} }
            }
        } catch (_: java.util.concurrent.RejectedExecutionException) {}
    }

    /** A new window size: the screen follows and the board is told (NAWS). */
    fun resize(cols: Int, rows: Int) {
        synchronized(screen) { screen.resize(cols, rows) }
        synchronized(telnet) { telnet.setSize(cols, rows) }
        listener.changed()
    }

    fun close() {
        ended = true
        try { sock?.close() } catch (_: IOException) {}
    }

    companion object {
        private val BRACKET = Regex("""\r\n\[([^\]\r\n]{1,100})\]\r\n""")

        /** What to show when the board closes: its last [message], or a refusal it printed. */
        fun reasonFrom(tail: String): String? {
            BRACKET.findAll(tail).lastOrNull()?.let { return "[" + it.groupValues[1] + "]" }
            for (m in listOf("try again later", "another login is in progress")) if (tail.contains(m)) return m
            return null
        }
    }
}
