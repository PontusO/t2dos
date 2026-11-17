package org.t2dos.term

/** How many character cells fit in a view: at least 20x4 (what edit needs), at most 1024x1024 (what telnetd takes). */
object Layout {
    fun grid(widthPx: Int, heightPx: Int, cellW: Float, cellH: Float): Pair<Int, Int> {
        val c = (widthPx / cellW).toInt().coerceIn(20, 1024)
        val r = (heightPx / cellH).toInt().coerceIn(4, 1024)
        return Pair(c, r)
    }
}
