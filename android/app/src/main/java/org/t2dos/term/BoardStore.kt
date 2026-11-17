package org.t2dos.term

import android.content.Context

/** A saved board.  `secret` is the password sealed by [Secret] (Base64), or null. */
data class Board(val id: Long, val name: String, val host: String, val port: Int, val fontSp: Float, val secret: String?)

/**
 * The saved boards as text: one board per line, fields separated by tabs, with backslash,
 * tab and newline escaped.  Kept in SharedPreferences; no JSON library needed.
 */
object BoardStore {
    fun encode(list: List<Board>): String = list.joinToString("\n") { b ->
        listOf(b.id.toString(), esc(b.name), esc(b.host), b.port.toString(), b.fontSp.toString(), esc(b.secret ?: "")).joinToString("\t")
    }

    fun decode(s: String): List<Board> = s.split('\n').filter { it.isNotEmpty() }.mapNotNull { line ->
        val f = line.split('\t')
        if (f.size < 6) null
        else Board(f[0].toLongOrNull() ?: return@mapNotNull null, unesc(f[1]), unesc(f[2]), f[3].toIntOrNull() ?: 23,
            f[4].toFloatOrNull() ?: 14f, unesc(f[5]).ifEmpty { null })
    }

    private fun esc(s: String) = s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")

    private fun unesc(s: String): String {
        val o = StringBuilder(s.length)
        var i = 0
        while (i < s.length) {
            val c = s[i]
            if (c == '\\' && i + 1 < s.length) {
                o.append(when (s[i + 1]) { 't' -> '\t'; 'n' -> '\n'; else -> s[i + 1] }); i += 2
            } else { o.append(c); i++ }
        }
        return o.toString()
    }

    private const val PREFS = "boards"
    private const val KEY = "list"

    fun load(ctx: Context): List<Board> = decode(ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, "") ?: "")
    fun save(ctx: Context, list: List<Board>) { ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY, encode(list)).apply() }
    fun get(ctx: Context, id: Long): Board? = load(ctx).firstOrNull { it.id == id }
    fun put(ctx: Context, b: Board) { val l = load(ctx).filter { it.id != b.id } + b; save(ctx, l.sortedBy { it.name.lowercase() }) }
    fun remove(ctx: Context, id: Long) = save(ctx, load(ctx).filter { it.id != id })
}
