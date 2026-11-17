package org.t2dos.term

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast

/** The saved boards: tap to connect, long-press to edit or delete, "Add board" for a new one. */
class MainActivity : Activity() {
    private lateinit var list: ListView
    private lateinit var empty: TextView
    private var boards = listOf<Board>()

    override fun onCreate(saved: Bundle?) {
        super.onCreate(saved)
        title = "T2DOS Terminal"
        list = ListView(this)
        empty = TextView(this).apply {
            text = "No boards yet. Add one with its address (the board's wifi command shows it)."
            setPadding(dp(20), dp(20), dp(20), dp(20))
        }
        val add = Button(this).apply { text = "Add board"; setOnClickListener { edit(null) } }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(add, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
            addView(empty)
            addView(list, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        root.setOnApplyWindowInsetsListener { v, insets -> padForBars(v, insets, ime = false); insets }
        setContentView(root)
        list.setOnItemClickListener { _, _, pos, _ ->
            startActivity(Intent(this, TerminalActivity::class.java).putExtra(TerminalActivity.EXTRA_BOARD, boards[pos].id))
        }
        list.setOnItemLongClickListener { _, _, pos, _ ->
            val b = boards[pos]
            AlertDialog.Builder(this).setTitle(b.name)
                .setItems(arrayOf("Edit", "Delete")) { _, which ->
                    if (which == 0) edit(b)
                    else AlertDialog.Builder(this).setMessage("Delete ${b.name}?")
                        .setPositiveButton("Delete") { _, _ -> BoardStore.remove(this, b.id); refresh() }
                        .setNegativeButton("Cancel", null).show()
                }.show()
            true
        }
    }

    override fun onResume() { super.onResume(); refresh() }

    private fun refresh() {
        boards = BoardStore.load(this)
        list.adapter = object : ArrayAdapter<String>(this, android.R.layout.simple_list_item_2, android.R.id.text1, boards.map { it.name }) {
            override fun getView(pos: Int, v: View?, parent: ViewGroup): View {
                val view = super.getView(pos, v, parent)
                val b = boards[pos]
                view.findViewById<TextView>(android.R.id.text2).text =
                    "${b.host}:${b.port}" + if (b.secret != null) "  (password saved)" else ""
                return view
            }
        }
        empty.visibility = if (boards.isEmpty()) View.VISIBLE else View.GONE
    }

    /** The add/edit form; b == null adds a board. */
    private fun edit(b: Board?) {
        fun field(hint: String, value: String, type: Int) = EditText(this).apply { this.hint = hint; setText(value); inputType = type }
        val name = field("name", b?.name ?: "", InputType.TYPE_CLASS_TEXT)
        val host = field("address (e.g. 192.168.1.150)", b?.host ?: "", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI)
        val port = field("port", (b?.port ?: 23).toString(), InputType.TYPE_CLASS_NUMBER)
        val pw = field(if (b?.secret != null) "password (saved; type to change)" else "password (optional, saved encrypted)", "",
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
        val forget = CheckBox(this).apply { text = "Forget the saved password"; visibility = if (b?.secret != null) View.VISIBLE else View.GONE }
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(8), dp(20), 0)
            addView(name); addView(host); addView(port); addView(pw); addView(forget)
        }
        AlertDialog.Builder(this)
            .setTitle(if (b == null) "Add board" else "Edit board")
            .setView(box)
            .setPositiveButton("Save") { _, _ ->
                val h = host.text.toString().trim()
                val p = port.text.toString().toIntOrNull()
                if (h.isEmpty() || p == null || p !in 1..65535) {
                    Toast.makeText(this, "An address and a port 1-65535 are needed", Toast.LENGTH_LONG).show()
                    return@setPositiveButton
                }
                val secret = when {
                    pw.text.isNotEmpty() -> Secret.seal(pw.text.toString())
                    forget.isChecked -> null
                    else -> b?.secret
                }
                BoardStore.put(this, Board(b?.id ?: System.currentTimeMillis(), name.text.toString().trim().ifEmpty { h }, h, p, b?.fontSp ?: 14f, secret))
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
}

/** Edge to edge (targetSdk 35+): pads a root view clear of the system bars, and of the keyboard when ime. */
fun padForBars(v: View, insets: WindowInsets, ime: Boolean) {
    if (Build.VERSION.SDK_INT >= 30) {
        var t = WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout()
        if (ime) t = t or WindowInsets.Type.ime()
        val i = insets.getInsets(t)
        v.setPadding(i.left, i.top, i.right, i.bottom)
    } else {
        @Suppress("DEPRECATION")
        v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
    }
}
