package org.t2dos.term

import android.app.Activity
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView

/**
 * One board's console: the terminal, the extra-keys row and, after a disconnect, a Reconnect bar.
 * The session itself lives in [Sessions] (kept running by [SessionService]): leaving this screen
 * keeps it open, coming back attaches to it again.  Disconnect (in the keys row or the
 * notification) ends it.
 */
class TerminalActivity : Activity() {
    private lateinit var board: Board
    private lateinit var term: TerminalView
    private lateinit var bar: LinearLayout
    private lateinit var barText: TextView
    private lateinit var ctrlButton: Button
    private var password: String? = null
    private var session: Session? = null      // the session shown, while it is Sessions.current
    private val viewer = object : Session.Listener {
        override fun changed() { if (shown()) term.outputArrived() }
        override fun closed(reason: String) { runOnUiThread { if (shown()) disconnected(reason) } }
    }
    private fun shown() = session != null && Sessions.current?.session === session

    override fun onCreate(saved: Bundle?) {
        super.onCreate(saved)
        board = BoardStore.get(this, intent.getLongExtra(EXTRA_BOARD, -1)) ?: run { finish(); return }
        title = board.name

        term = TerminalView(this).apply {
            fontSp = board.fontSp
            onFontChanged = { sp -> board = board.copy(fontSp = sp); BoardStore.put(this@TerminalActivity, board) }
            onCtrlChanged = { st -> paintCtrl(st) }
        }
        barText = TextView(this).apply { setTextColor(Color.WHITE); setPadding(dp(12), 0, dp(12), 0) }
        val reconnect = Button(this).apply { text = "Reconnect"; setOnClickListener { connect() } }
        bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Color.rgb(0x60, 0x20, 0x20)); visibility = View.GONE
            addView(barText, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(reconnect)
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(TerminalView.BG)
            addView(bar, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
            addView(term, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(keysRow(), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }
        root.setOnApplyWindowInsetsListener { v, insets -> padForBars(v, insets, ime = true); insets }   // keyboard shrinks the terminal
        setContentView(root)
        SessionService.install(this)
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf("android.permission.POST_NOTIFICATIONS"), 1)   // for the "connected" notification; works without it

        val open = Sessions.current
        if (open != null && open.board.id == board.id) {      // still open from before: show it again
            password = open.password
            show(open.session)
            open.closedReason?.let { disconnected(it) }
            SessionService.refresh(this)
            return
        }
        val sealed = board.secret
        password = sealed?.let { Secret.open(it) }
        if (password != null) connect() else askPassword(sealed != null)
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, perms, results)
        if (results.firstOrNull() == PackageManager.PERMISSION_GRANTED) SessionService.refresh(this)   // post it now
    }

    override fun onStart() { super.onStart(); Sessions.attach(viewer) }
    override fun onStop() { Sessions.detach(viewer); super.onStop() }

    private fun show(se: Session) {
        session = se
        term.session = se
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        bar.visibility = View.GONE
        if (hasWindowFocus()) term.showKeyboard()
    }

    private fun askPassword(couldNotOpen: Boolean) {
        val pw = EditText(this).apply { inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD; hint = "password" }
        val remember = CheckBox(this).apply { text = "Remember on this phone" }
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(8), dp(20), 0)
            if (couldNotOpen) addView(TextView(this@TerminalActivity).apply { text = "The saved password could not be read; enter it again." })
            addView(pw); addView(remember)
        }
        AlertDialog.Builder(this)
            .setTitle(board.name)
            .setView(box)
            .setPositiveButton("Connect") { _, _ ->
                val p = pw.text.toString()
                password = p.ifEmpty { null }
                if (password != null && remember.isChecked) { board = board.copy(secret = Secret.seal(p)); BoardStore.put(this, board) }
                connect()
            }
            .setNegativeButton("Cancel") { _, _ -> finish() }
            .setOnCancelListener { finish() }
            .show()
    }

    private fun connect() {
        if (term.width == 0) { term.post { connect() }; return }
        val (c, r) = term.gridFor(term.width, term.height)
        show(Sessions.connect(board, password, c, r).session)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {      // the keyboard can only open once the window has focus
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && session != null && bar.visibility != View.VISIBLE) term.showKeyboard()
    }

    private fun disconnected(reason: String) {
        if (isFinishing) return
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        barText.text = "Disconnected: $reason"
        bar.visibility = View.VISIBLE
    }

    private fun keysRow(): View {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        fun add(label: String, action: () -> Unit): Button {
            val b = Button(this).apply {
                text = label; isAllCaps = false; isFocusable = false; minWidth = 0; minimumWidth = dp(44)
                setPadding(dp(8), 0, dp(8), 0)
                setOnClickListener { action() }
            }
            row.addView(b)
            return b
        }
        add("Esc") { term.sendKey(Key.ESC) }
        ctrlButton = add("Ctrl") { term.ctrl = (term.ctrl + 1) % 3 }    // tap: next key; tap again: locked; again: off
        add("Tab") { term.sendKey(Key.TAB) }
        add("←") { term.sendKey(Key.LEFT) }
        add("↑") { term.sendKey(Key.UP) }
        add("↓") { term.sendKey(Key.DOWN) }
        add("→") { term.sendKey(Key.RIGHT) }
        add("Home") { term.sendKey(Key.HOME) }
        add("End") { term.sendKey(Key.END) }
        add("PgUp") { term.sendKey(Key.PGUP) }
        add("PgDn") { term.sendKey(Key.PGDN) }
        add("Del") { term.sendKey(Key.DEL) }
        add("^C") { session?.send(byteArrayOf(3)) }
        add("Disconnect") { Sessions.disconnect(); finish() }
        return HorizontalScrollView(this).apply { isHorizontalScrollBarEnabled = false; addView(row) }
    }

    private fun paintCtrl(st: Int) {
        ctrlButton.text = when (st) { 1 -> "CTRL"; 2 -> "CTRL🔒"; else -> "Ctrl" }
        ctrlButton.setTextColor(if (st == 0) Color.LTGRAY else Color.rgb(0x4e, 0xc9, 0x6b))
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    companion object { const val EXTRA_BOARD = "board" }
}
