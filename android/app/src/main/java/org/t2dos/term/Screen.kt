package org.t2dos.term

/** One character cell: fg/bg are ANSI colours 0..7. */
data class Cell(val ch: Char, val fg: Int, val bg: Int, val dim: Boolean, val reverse: Boolean)

/**
 * The terminal screen for what a T2DOS board sends (see the design's screen-code table): text,
 * CR LF BS TAB, cursor moves, erase, colours/dim/reverse, cursor on/off, the alternate screen
 * (edit's), and the cursor position report.  One byte is one cell, shown as Latin-1 (128-159 as
 * a dot).  Unknown sequences are swallowed.  Plain Kotlin; the caller serialises access.
 */
class Screen(cols: Int, rows: Int, val reply: (ByteArray) -> Unit) {
    var cols = cols; private set
    var rows = rows; private set
    var cursorRow = 0; private set
    var cursorCol = 0; private set
    var cursorVisible = true; private set
    var altScreen = false; private set
    /** Moves on every change; the view redraws when it differs from what it drew. */
    var version = 0L

    // a cell is packed: char in the low 16 bits, attributes above (fg, bg, dim, reverse)
    private var main = Array(rows) { blankRow(cols) }
    private var alt = Array(rows) { blankRow(cols) }
    private val grid get() = if (altScreen) alt else main
    private val history = ArrayDeque<IntArray>()      // newest first
    private var attr = DEFAULT
    private var wrapPending = false
    private var savedRow = 0; private var savedCol = 0

    private enum class St { TEXT, ESC, CSI, OSC, OSC_ESC }
    private var st = St.TEXT
    private val params = IntArray(16)
    private var np = 0
    private var priv = false

    val scrollback: Int get() = history.size
    fun historyRowText(i: Int): String = text(history[i])
    fun historyCell(i: Int, col: Int): Cell { val r = history[i]; return unpack(if (col < r.size) r[col] else BLANK) }
    fun cell(row: Int, col: Int): Cell = unpack(grid[row][col])
    fun rowText(row: Int): String = text(grid[row])

    fun feed(data: ByteArray, off: Int = 0, len: Int = data.size) {
        for (i in off until off + len) byte(data[i].toInt() and 0xff)
        version++
    }

    private fun byte(c: Int) {
        when (st) {
            St.TEXT -> when {
                c == 27 -> st = St.ESC
                c == 13 -> { cursorCol = 0; wrapPending = false }
                c == 10 -> { lineFeed(); wrapPending = false }
                c == 8 -> { if (cursorCol > 0) cursorCol--; wrapPending = false }
                c == 9 -> { cursorCol = minOf(cols - 1, (cursorCol / 8 + 1) * 8); wrapPending = false }
                c < 32 || c == 127 -> {}
                else -> put(if (c in 128..159) '·' else c.toChar())
            }
            St.ESC -> when (c) {
                '['.code -> { st = St.CSI; np = 0; params.fill(0); priv = false }
                ']'.code -> st = St.OSC
                else -> st = St.TEXT                      // a two-byte escape: nothing T2DOS uses
            }
            St.CSI -> when {
                c in '0'.code..'9'.code -> { if (np == 0) np = 1; if (np <= params.size) params[np - 1] = minOf(params[np - 1] * 10 + c - '0'.code, 9999) }
                c == ';'.code -> { if (np == 0) np = 1; np++ }
                c == '?'.code -> priv = true
                c in 0x40..0x7e -> { st = St.TEXT; csi(c.toChar()) }
                c in 0x20..0x3f -> {}                     // other parameter/intermediate bytes
                else -> st = St.TEXT
            }
            St.OSC -> if (c == 7) st = St.TEXT else if (c == 27) st = St.OSC_ESC
            St.OSC_ESC -> st = if (c == '\\'.code) St.TEXT else St.OSC
        }
    }

    private fun p(i: Int, def: Int): Int = if (i < np && i < params.size && params[i] != 0) params[i] else def

    private fun csi(f: Char) {
        wrapPending = wrapPending && f == 'm'
        when (f) {
            'H', 'f' -> { cursorRow = (p(0, 1) - 1).coerceIn(0, rows - 1); cursorCol = (p(1, 1) - 1).coerceIn(0, cols - 1) }
            'A' -> cursorRow = maxOf(0, cursorRow - p(0, 1))
            'B' -> cursorRow = minOf(rows - 1, cursorRow + p(0, 1))
            'C' -> cursorCol = minOf(cols - 1, cursorCol + p(0, 1))
            'D' -> cursorCol = maxOf(0, cursorCol - p(0, 1))
            'J' -> when (p(0, 0)) {
                0 -> { clear(cursorRow, cursorCol, cols); for (r in cursorRow + 1 until rows) clear(r, 0, cols) }
                1 -> { for (r in 0 until cursorRow) clear(r, 0, cols); clear(cursorRow, 0, cursorCol + 1) }
                else -> for (r in 0 until rows) clear(r, 0, cols)
            }
            'K' -> when (p(0, 0)) {
                0 -> clear(cursorRow, cursorCol, cols)
                1 -> clear(cursorRow, 0, cursorCol + 1)
                else -> clear(cursorRow, 0, cols)
            }
            'm' -> sgr()
            'n' -> if (!priv && p(0, 0) == 6) reply("\u001b[${cursorRow + 1};${cursorCol + 1}R".toByteArray())
            'h', 'l' -> if (priv) mode(p(0, 0), f == 'h')
        }
    }

    private fun sgr() {
        val n = maxOf(np, 1)
        for (i in 0 until minOf(n, params.size)) {
            val v = params[i]
            attr = when (v) {
                0 -> DEFAULT
                2 -> attr or DIM
                7 -> attr or REV
                22 -> attr and DIM.inv()
                27 -> attr and REV.inv()
                in 30..37 -> (attr and FG_MASK.inv()) or ((v - 30) shl FG_SHIFT)
                39 -> (attr and FG_MASK.inv()) or (7 shl FG_SHIFT)
                in 40..47 -> (attr and BG_MASK.inv()) or ((v - 40) shl BG_SHIFT)
                49 -> attr and BG_MASK.inv()
                in 90..97 -> (attr and FG_MASK.inv()) or ((v - 90) shl FG_SHIFT)
                in 100..107 -> (attr and BG_MASK.inv()) or ((v - 100) shl BG_SHIFT)
                else -> attr
            }
        }
    }

    private fun mode(m: Int, on: Boolean) {
        when (m) {
            25 -> cursorVisible = on
            47, 1047, 1049 -> if (on != altScreen) {
                if (on) {
                    savedRow = cursorRow; savedCol = cursorCol
                    altScreen = true
                    for (r in 0 until rows) clear(r, 0, cols)
                } else {
                    altScreen = false
                    cursorRow = savedRow.coerceIn(0, rows - 1); cursorCol = savedCol.coerceIn(0, cols - 1)
                }
            }
        }
    }

    private fun put(ch: Char) {
        if (wrapPending) { cursorCol = 0; lineFeed(); wrapPending = false }
        grid[cursorRow][cursorCol] = ch.code or attr
        if (cursorCol == cols - 1) wrapPending = true else cursorCol++
    }

    private fun lineFeed() {
        if (cursorRow < rows - 1) { cursorRow++; return }
        scrollUp()
    }

    private fun scrollUp() {
        val g = grid
        val top = g[0]
        if (!altScreen) { history.addFirst(top); if (history.size > HISTORY) history.removeLast() }
        for (r in 0 until rows - 1) g[r] = g[r + 1]
        g[rows - 1] = blankRow(cols)
    }

    private fun clear(row: Int, from: Int, to: Int) {
        val r = grid[row]
        for (c in from until minOf(to, cols)) r[c] = BLANK or (attr and BG_MASK)
    }

    /** New size: keeps the text top-left, but scrolls up so the cursor's row stays visible. */
    fun resize(newCols: Int, newRows: Int) {
        if (newCols == cols && newRows == rows) return
        val shift = maxOf(0, cursorRow - (newRows - 1))
        for (s in 0 until shift) { if (!altScreen) { history.addFirst(main[s]); if (history.size > HISTORY) history.removeLast() } }
        main = remap(main, if (altScreen) 0 else shift, newCols, newRows)
        alt = remap(alt, if (altScreen) shift else 0, newCols, newRows)
        cols = newCols; rows = newRows
        cursorRow = (cursorRow - shift).coerceIn(0, rows - 1); cursorCol = cursorCol.coerceIn(0, cols - 1)
        savedRow = savedRow.coerceIn(0, rows - 1); savedCol = savedCol.coerceIn(0, cols - 1)
        wrapPending = false
        version++
    }

    private fun remap(g: Array<IntArray>, from: Int, nc: Int, nr: Int): Array<IntArray> =
        Array(nr) { r ->
            val src = g.getOrNull(r + from)
            IntArray(nc) { c -> if (src != null && c < src.size) src[c] else BLANK }
        }

    private fun text(r: IntArray): String {
        val sb = StringBuilder(r.size)
        for (v in r) sb.append((v and 0xffff).toChar())
        return sb.trimEnd(' ').toString()
    }

    private fun blankRow(n: Int) = IntArray(n) { BLANK }

    private fun unpack(v: Int) = Cell((v and 0xffff).toChar(), (v shr FG_SHIFT) and 7, (v shr BG_SHIFT) and 7, v and DIM != 0, v and REV != 0)

    companion object {
        const val HISTORY = 1000
        private const val FG_SHIFT = 16
        private const val BG_SHIFT = 19
        private const val FG_MASK = 7 shl FG_SHIFT
        private const val BG_MASK = 7 shl BG_SHIFT
        private const val DIM = 1 shl 22
        private const val REV = 1 shl 23
        private const val DEFAULT = 7 shl FG_SHIFT
        private const val BLANK = ' '.code or DEFAULT
    }
}
