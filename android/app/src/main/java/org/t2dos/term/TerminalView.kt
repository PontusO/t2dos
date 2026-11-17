package org.t2dos.term

import android.content.ClipboardManager
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.text.InputType
import android.util.TypedValue
import android.view.GestureDetector
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import kotlin.math.ceil

/**
 * Draws a session's [Screen] and turns the phone keyboard (and a hardware one) into the bytes
 * the board expects.  Vertical drag scrolls back through the shell's history, pinch changes the
 * font size, tap shows the keyboard, long-press pastes.
 */
class TerminalView(context: Context) : View(context) {
    var session: Session? = null
        set(v) { field = v; scrollOffset = 0; fitGrid(force = true); invalidate() }

    /** Ctrl from the extra-keys row: 0 off, 1 for the next key, 2 locked. */
    var ctrl = 0
        set(v) { field = v; onCtrlChanged(v) }
    var onCtrlChanged: (Int) -> Unit = {}
    var onFontChanged: (Float) -> Unit = {}

    var fontSp = 14f
        set(v) { field = v.coerceIn(8f, 32f); measureCell(); fitGrid(force = false); invalidate() }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Typeface.MONOSPACE }
    private var cellW = 1f
    private var cellH = 1f
    private var baseline = 0f
    private var scrollOffset = 0          // history lines shown above the screen, 0 = at the bottom
    private var dragRest = 0f
    private val runText = CharArray(1024)

    init {
        isFocusable = true; isFocusableInTouchMode = true
        defaultFocusHighlightEnabled = false      // else Android greys the whole focused view
        measureCell()
    }

    private fun measureCell() {
        paint.textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, fontSp, resources.displayMetrics)
        cellW = paint.measureText("M")
        val fm = paint.fontMetrics
        cellH = ceil(fm.descent - fm.ascent)
        baseline = -fm.ascent
    }

    /** The grid for the current view size; tells the session when it changed. */
    private fun fitGrid(force: Boolean) {
        if (width == 0 || height == 0) return
        val (c, r) = Layout.grid(width, height, cellW, cellH)
        val se = session ?: return
        val now = synchronized(se.screen) { Pair(se.screen.cols, se.screen.rows) }
        if (force || now != Pair(c, r)) se.resize(c, r)
    }

    fun gridFor(w: Int, h: Int) = Layout.grid(w, h, cellW, cellH)

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) { fitGrid(force = false) }

    /** New output: back to the bottom and redraw (any thread). */
    fun outputArrived() { post { scrollOffset = 0; invalidate() } }

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(BG)
        val se = session ?: return
        synchronized(se.screen) {
            val s = se.screen
            scrollOffset = scrollOffset.coerceIn(0, if (s.altScreen) 0 else s.scrollback)
            for (v in 0 until s.rows) {
                val hist = v < scrollOffset
                val row = if (hist) scrollOffset - 1 - v else v - scrollOffset
                drawRow(canvas, s, v, row, hist)
            }
            if (s.cursorVisible && scrollOffset == 0 && s.cursorRow + scrollOffset < s.rows) {
                val x = s.cursorCol * cellW; val y = (s.cursorRow + scrollOffset) * cellH
                paint.color = FG_CURSOR
                canvas.drawRect(x, y, x + cellW, y + cellH, paint)
                val c = s.cell(s.cursorRow, s.cursorCol)
                if (c.ch != ' ') { paint.color = BG; canvas.drawText(charArrayOf(c.ch), 0, 1, x, y + baseline, paint) }
            }
        }
    }

    private fun drawRow(canvas: Canvas, s: Screen, v: Int, row: Int, hist: Boolean) {
        val y = v * cellH
        var col = 0
        while (col < s.cols) {
            val first = if (hist) s.historyCell(row, col) else s.cell(row, col)
            var n = 0
            var c = col
            while (c < s.cols && n < runText.size) {
                val cl = if (hist) s.historyCell(row, c) else s.cell(row, c)
                if (cl.fg != first.fg || cl.bg != first.bg || cl.dim != first.dim || cl.reverse != first.reverse) break
                runText[n++] = cl.ch; c++
            }
            var fg = PALETTE[first.fg]; var bg = if (first.bg == 0) BG else PALETTE[first.bg]
            if (first.reverse) { val t = fg; fg = bg; bg = t }
            if (bg != BG) { paint.color = bg; canvas.drawRect(col * cellW, y, (col + n) * cellW, y + cellH, paint) }
            paint.color = fg
            if (first.dim) paint.alpha = 0x80
            canvas.drawText(runText, 0, n, col * cellW, y + baseline, paint)
            paint.alpha = 0xff
            col += n
        }
    }

    // ---------------------------------------------------------------- touch
    private val gestures = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(e: MotionEvent) = true
        override fun onSingleTapUp(e: MotionEvent): Boolean { showKeyboard(); return true }
        override fun onLongPress(e: MotionEvent) { paste() }
        override fun onScroll(e1: MotionEvent?, e2: MotionEvent, dx: Float, dy: Float): Boolean {
            dragRest -= dy
            val lines = (dragRest / cellH).toInt()
            if (lines != 0) { dragRest -= lines * cellH; scrollOffset += lines; invalidate() }
            return true
        }
    })

    private val scaler = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(d: ScaleGestureDetector): Boolean { fontSp *= d.scaleFactor; return true }
        override fun onScaleEnd(d: ScaleGestureDetector) { onFontChanged(fontSp) }
    })

    override fun onTouchEvent(e: MotionEvent): Boolean {
        scaler.onTouchEvent(e)
        if (!scaler.isInProgress) gestures.onTouchEvent(e)
        return true
    }

    fun showKeyboard() {
        requestFocus()
        (context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun paste() {
        val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val t = cm.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString() ?: return
        sendBytes(Keys.text(t, false))
    }

    // ---------------------------------------------------------------- keys
    private fun sendBytes(b: ByteArray) {
        if (b.isEmpty()) return
        session?.send(b)
        if (scrollOffset != 0) { scrollOffset = 0; invalidate() }
    }

    /** A key from the extra-keys row or the keyboard. */
    fun sendKey(k: Key) { sendBytes(Keys.key(k)); if (ctrl == 1) ctrl = 0 }

    /** Typed text: with Ctrl on, letters become control codes. */
    fun sendText(s: String) {
        if (s.isEmpty()) return
        sendBytes(Keys.text(s, ctrl != 0))
        if (ctrl == 1) ctrl = 0
    }

    private fun keyFor(code: Int): Key? = when (code) {
        KeyEvent.KEYCODE_DEL -> Key.BACKSPACE
        KeyEvent.KEYCODE_FORWARD_DEL -> Key.DEL
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> Key.ENTER
        KeyEvent.KEYCODE_DPAD_UP -> Key.UP
        KeyEvent.KEYCODE_DPAD_DOWN -> Key.DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> Key.LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> Key.RIGHT
        KeyEvent.KEYCODE_MOVE_HOME -> Key.HOME
        KeyEvent.KEYCODE_MOVE_END -> Key.END
        KeyEvent.KEYCODE_PAGE_UP -> Key.PGUP
        KeyEvent.KEYCODE_PAGE_DOWN -> Key.PGDN
        KeyEvent.KEYCODE_ESCAPE -> Key.ESC
        KeyEvent.KEYCODE_TAB -> Key.TAB
        else -> null
    }

    /** A key event from a hardware keyboard or an IME; true if it was used. */
    fun handleKey(e: KeyEvent): Boolean {
        if (e.action != KeyEvent.ACTION_DOWN) return keyFor(e.keyCode) != null || e.unicodeChar != 0
        keyFor(e.keyCode)?.let { sendKey(it); return true }
        val u = e.getUnicodeChar(e.metaState and KeyEvent.META_CTRL_MASK.inv())
        if (u == 0) return false
        val s = String(Character.toChars(u))
        if (e.isCtrlPressed) { sendBytes(Keys.text(s, true)); if (ctrl == 1) ctrl = 0 } else sendText(s)
        return true
    }

    override fun onKeyDown(keyCode: Int, e: KeyEvent): Boolean = handleKey(e) || super.onKeyDown(keyCode, e)
    override fun onKeyUp(keyCode: Int, e: KeyEvent): Boolean = keyFor(keyCode) != null || super.onKeyUp(keyCode, e)

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(out: EditorInfo): InputConnection {
        out.inputType = InputType.TYPE_NULL
        out.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN
        return object : BaseInputConnection(this, false) {
            private var composing = ""
            override fun commitText(text: CharSequence, pos: Int): Boolean { composing = ""; sendText(text.toString()); return true }
            override fun setComposingText(text: CharSequence, pos: Int): Boolean { composing = text.toString(); return true }
            override fun finishComposingText(): Boolean { val c = composing; composing = ""; sendText(c); return true }
            override fun deleteSurroundingText(before: Int, after: Int): Boolean {
                repeat(before) { sendKey(Key.BACKSPACE) }; repeat(after) { sendKey(Key.DEL) }; return true
            }
            override fun sendKeyEvent(e: KeyEvent): Boolean { handleKey(e); return true }
        }
    }

    companion object {
        val BG = Color.rgb(0x12, 0x12, 0x12)
        val FG_CURSOR = Color.rgb(0xd0, 0xd0, 0xd0)
        val PALETTE = intArrayOf(
            Color.rgb(0x12, 0x12, 0x12), Color.rgb(0xe0, 0x55, 0x55), Color.rgb(0x4e, 0xc9, 0x6b), Color.rgb(0xe5, 0xc0, 0x45),
            Color.rgb(0x4a, 0x8f, 0xe0), Color.rgb(0xc0, 0x6c, 0xd8), Color.rgb(0x3f, 0xbf, 0xcf), Color.rgb(0xd0, 0xd0, 0xd0),
        )
    }
}
