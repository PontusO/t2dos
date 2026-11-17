package org.t2dos.term

import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * Drives the real T2DOS shell and edit under qemu (host_linux with telnetd on).  Skipped unless
 * T2DOS_E2E_PORT is set: tools/check_android.py starts the board software and sets it.
 */
class BoardE2ETest {
    private val port = System.getenv("T2DOS_E2E_PORT")?.toIntOrNull()
    private val pass = System.getenv("T2DOS_E2E_PASS") ?: "sesame"

    private fun connect(): Session {
        assumeTrue("T2DOS_E2E_PORT not set", port != null)
        val se = Session("127.0.0.1", port!!, pass, 80, 24, object : Session.Listener {
            override fun changed() {}
            override fun closed(reason: String) {}
        })
        se.start()
        waitFor(se, "login") { text(it).contains("Press Enter for a prompt.") }
        se.send(Keys.key(Key.ENTER))
        waitFor(se, "prompt") { it.rowText(it.cursorRow).endsWith("/>") }
        return se
    }

    private fun text(s: Screen) = (0 until s.rows).joinToString("\n") { s.rowText(it) }

    private fun waitFor(se: Session, what: String, ms: Long = 10000, ok: (Screen) -> Boolean) {
        val t0 = System.currentTimeMillis()
        while (System.currentTimeMillis() - t0 < ms) {
            if (synchronized(se.screen) { ok(se.screen) }) return
            Thread.sleep(20)
        }
        val now = synchronized(se.screen) { text(se.screen) }
        throw AssertionError("$what: not seen in\n$now")
    }

    private fun type(se: Session, s: String) = se.send(Keys.text(s, false))
    private fun run(se: Session, cmd: String) { type(se, cmd); se.send(Keys.key(Key.ENTER)) }

    @Test fun shell() {
        val se = connect()
        run(se, "ls /bin")
        waitFor(se, "ls /bin") { text(it).contains("edit.bin") && it.rowText(it.cursorRow).endsWith("/>") }
        se.close()
    }

    @Test fun catBig() {
        val se = connect()
        val last = System.getenv("T2DOS_E2E_LAST") ?: error("T2DOS_E2E_LAST not set")
        run(se, "cat /tmp/e2e.txt")
        waitFor(se, "end of cat") { it.cursorRow > 0 && it.rowText(it.cursorRow).endsWith("/>") && it.rowText(it.cursorRow - 1) == last }
        se.close()
    }

    @Test fun editTypeSaveQuit() {
        val se = connect()
        run(se, "rm /tmp/e2e2.txt")
        waitFor(se, "rm") { it.rowText(it.cursorRow).endsWith("/>") && text(it).contains("rm /tmp/e2e2.txt") }
        run(se, "edit /tmp/e2e2.txt")
        waitFor(se, "edit open") { it.altScreen && it.rowText(it.rows - 1).startsWith(" /tmp/e2e2.txt") }
        type(se, "hello")
        waitFor(se, "typed") { it.rowText(0) == "hello" }
        se.send(byteArrayOf(19)); se.send(byteArrayOf(24))                  // ^S ^X
        waitFor(se, "edit quit") { !it.altScreen && it.rowText(it.cursorRow).endsWith("/>") }
        run(se, "cat /tmp/e2e2.txt")
        waitFor(se, "saved") { it.rowText(it.cursorRow) == "hello/>" }       // no newline after hello
        se.close()
    }

    @Test fun editResize() {
        val se = connect()
        run(se, "edit /tmp/e2e3.txt")
        waitFor(se, "edit open") { it.altScreen && it.rowText(23).startsWith(" /tmp/e2e3.txt") }
        se.resize(60, 20)
        waitFor(se, "redraw at 60x20", 1000) { it.rows == 20 && it.rowText(19).startsWith(" /tmp/e2e3.txt") }
        assertEquals(20, synchronized(se.screen) { se.screen.rows })
        se.send(byteArrayOf(24))
        waitFor(se, "edit quit") { !it.altScreen }
        se.close()
    }
}
