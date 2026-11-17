package org.t2dos.term

/**
 * The one open session, kept outside any screen so it lives on while the app is in the
 * background (SessionService keeps the process running while there is one).  A screen attaches
 * to it while visible; output and the close arrive at the attached screen, and a screen that
 * attaches after the close is told the reason at once.  Plain Kotlin: the service hooks in
 * through onActive.
 */
object Sessions {
    class Live(val board: Board, val password: String?, cols: Int, rows: Int) {
        @Volatile var closedReason: String? = null
        val session: Session = Session(board.host, board.port, password, cols, rows, object : Session.Listener {
            override fun changed() { if (current === this@Live) viewer?.changed() }
            override fun closed(reason: String) {
                closedReason = reason
                if (current === this@Live) { viewer?.closed(reason); onActive(false) }
            }
        })
    }

    @Volatile var current: Live? = null
        private set
    @Volatile private var viewer: Session.Listener? = null

    /** Told true when a session opens and false when the open one ends (starts/stops the service). */
    @Volatile var onActive: (Boolean) -> Unit = {}

    /** Opens a session to board, closing any other one (the app has one at a time). */
    fun connect(board: Board, password: String?, cols: Int, rows: Int): Live {
        val live = Live(board, password, cols, rows)
        val old: Live?
        synchronized(this) { old = current; current = live }
        old?.session?.close()
        onActive(true)
        live.session.start()
        return live
    }

    /** Ends the open session (the Disconnect button). */
    fun disconnect() {
        val c: Live?
        synchronized(this) { c = current; current = null }
        if (c != null) { c.session.close(); onActive(false) }
    }

    fun attach(l: Session.Listener) {
        viewer = l
        current?.closedReason?.let { l.closed(it) }
    }

    fun detach(l: Session.Listener) { if (viewer === l) viewer = null }
}
