# T2DOS Android Terminal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A free Android app ("T2DOS Terminal") that logs in to the board's telnetd and runs the shell, `edit` and other full-screen programs, plus the two board-side changes it needs.

**Architecture:** Board side: `edit` redraws when the console size changes, and `host_linux` forwards `conctl()` to the telnet session like the board. App side: plain-Kotlin `Telnet`, `Screen` and `Keys` classes (JVM-tested, no Android types), a `Session` that runs the socket, and an Android UI (`TerminalView`, extra-keys row, boards list). An end-to-end JVM test drives the real shell and `edit` under qemu.

**Tech Stack:** C (T2DOS subset) for `edit.c`, C for `host_linux.c`, Python 3 for `tools/`, Kotlin + Android SDK 36 (AGP 9.1.0, Gradle 9.3.1, JUnit 4 for JVM tests), no other libraries.

**Spec:** `docs/superpowers/specs/2026-11-17-android-terminal-design.md`

## Global Constraints

- No outside libraries beyond the Android SDK and AndroidX basics; JUnit 4 for JVM tests only.
- Package id `org.t2dos.term`, app name "T2DOS Terminal", minSdk 26, compileSdk/targetSdk 36.
- The app lives in `android/` in this repo; Gradle wrapper 9.3.1; builds must work with `--offline` (AGP 9.1.0 and Kotlin 2.3.10 are in `~/.gradle/caches`).
- telnetd (`kernel/telnetd.c`) does not change.
- One byte is one cell, shown as Latin-1; 128-159 show as a placeholder glyph (U+00B7 `·`). Typed characters 160-255 are sent as one byte; other non-ASCII characters are not sent.
- Keys sent: arrows `ESC[A/B/C/D`, Home `ESC[H`, End `ESC[F`, PgUp `ESC[5~`, PgDn `ESC[6~`, Del `ESC[3~`, Esc 27, Tab 9, Backspace 127, Enter CR (on the wire CR NUL), Ctrl+letter 1..26, `^C` 3.
- A saved password is encrypted with an Android Keystore key, never stored in plain text. Auto-login types it once per connection, on `password: `; after `wrong password` it stops.
- No automatic reconnect.
- `edit.c` stays inside the subset the committed `cc` compiles (no `static`, `const`, initialisers etc.; see CLAUDE.md), and every other userland binary must stay byte-identical (`make` then compare `root/bin`).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and nothing else; no em dashes anywhere.
- Board use: the board's serial port is the user's; if `fuser /dev/ttyACM*` shows `screen`, stop and ask. `tools/check_telnetd.py --board` replaces the user's telnetd password: do not run it without asking.

## Review Focus

1. An escape or IAC sequence split across two socket reads (`ESC` | `[2J`, `IAC` | `IAC`): must draw or decode exactly as if it came in one read. Tests in Tasks 2 and 3.
2. A line exactly as wide as the screen followed by CR LF (edit's status line, `ls` output at width 80): one line, not a blank line after it (pending wrap). Test in Task 3.
3. The Android keyboard (Gboard) with predictions, composing text and its own Backspace: each typed letter must reach the board at once, Backspace must send 127, Enter CR. Covered by the `InputConnection` rules in Task 6 and the `Keys` tests in Task 4.
4. The connection drops mid-output (WiFi off, board reset, takeover): the app keeps the last screen, shows "Disconnected" with the reason, and does not crash or hang. Test in Task 4 (`Session` against a server that closes).
5. A big burst of output (`cat` of a 60 KB file): the UI stays responsive and the screen ends correct. Test in Task 3 (200 KB through `Screen` under 1 s on the JVM) and Task 5 (cat through the real shell).

---

### Task 1: Board side: edit follows size changes; host_linux forwards conctl

**Files:**
- Modify: `userland/edit.c` (`getkey`, `term_size`, `main` loop; new `K_RESIZE`)
- Modify: `host_linux.c:306` (`SYS_conctl`)
- Modify: `README.md` (`edit` section: one sentence that edit follows window size changes from a telnet client)
- Create: `tests/telnetd/ctl.c`
- Test: `tools/check_telnetd.py` (new `edit_check()` run from `pc_main()` after `scenario()`; the board path does not call it)

**Interfaces:**
- Consumes: telnetd as is; `sysinfo(INFO_CONSOLE)` = width | height << 16 (80x24 when no client size).
- Produces: `edit` redraws at the new size within 0.5 s of a NAWS change with no key pressed; `host_linux` writes `conctl` codes to stdout and the session as `kernel.ino` `SerialConsole::ansi` does (`ESC[2J ESC[H`, `ESC[r;cH`, `ESC[3x;4xm`).

- [ ] **Step 1: Write the failing tests** in `tools/check_telnetd.py`, plus `tests/telnetd/ctl.c` (built to `/tmp/tdctl.bin` the same way `size.c` becomes `/tmp/tdsize.bin`):

```c
// tests/telnetd/ctl.c: conctl codes must reach a telnet session
#include "sys.h"
int main() { conctl(CON_GOTO, 2, 3); conctl(CON_COLOR, 1, 4); printf("ctl\n"); return 0; }
```

```python
def edit_check(con, host, port):
    c = Client(host, port); c.login('edit')
    c.send(bytes([IAC, SB, 31, 0, 100, 0, 30, IAC, SE]))
    c.send('/tmp/tdctl.bin\r\n')
    c.r.wait(b'\x1b[4;3H\x1b[31;44mctl\r\n', name='conctl reaches the session')
    c.send('edit /tmp/rs.txt\r\n')
    c.r.wait(b'\x1b[30;1H', 5, 'edit status line at row 30')       # 100x30
    mark = len(c.r.seen)
    c.send(bytes([IAC, SB, 31, 0, 60, 0, 20, IAC, SE]))             # no key pressed
    c.r.wait(b'\x1b[20;1H', 2, 'edit redraws at row 20')             # must appear after mark
    if c.r.seen.rfind(b'\x1b[20;1H') < mark: fail('no redraw after the resize')
    c.send('abc'); c.send(bytes([19])); c.send(bytes([24]))          # type, ^S, ^X
    c.r.wait(b'/> ', 5, 'edit quit'); c.close()
    con.type('cat /tmp/rs.txt\r'); con.r.wait(b'abc', name='edit saved after resize')
```

(Adjust the expected status-line sequence to what `status()` actually emits for row H; read `edit.c:105-117` first and assert that exact bytes.)

- [ ] **Step 2: Run to see both fail**

Run: `make && make host && python3 tools/check_telnetd.py --pc`
Expected: FAIL `conctl reaches the session`, and (after temporarily skipping it) FAIL `edit redraws at row 20`.

- [ ] **Step 3: host_linux `SYS_conctl`**: `CON_BREAK` stays a no-op returning 0; other commands build the same string as `SerialConsole::ansi` (copy the three formats) and send it through `k_write(1, ...)`, which already reaches stdout and `td_putc`/`td_flush`.

- [ ] **Step 4: edit**: add `K_RESIZE` next to the other `K_` codes and a global `int csz;` (the last `sysinfo(INFO_CONSOLE)` value, set in `term_size()`). `getkey()` waits for the first byte with `while (!kbhit()) { if (sysinfo(INFO_CONSOLE) != csz) return K_RESIZE; sleep(20); }` before its `getchar()`. In `main`, `K_RESIZE` sets `csz`, `cols = csz & 65535`, `H = csz >> 16` (same `< 20` / `< 4` floors as `term_size`), `rows = H - 1`, `left = 0`, and the loop's next `draw()` repaints everything (clear first: `csi("2J")`). It is not a key: `msg`, `quitarm`, `lastk`, `lastcr` stay as they were. USB without a session never changes `INFO_CONSOLE` (always 80x24), so USB behaviour is unchanged.

- [ ] **Step 5: Run the tests**

Run: `make && make host && python3 tools/check_telnetd.py --pc && make check && python3 tools/check_ref.py && python3 tools/check_sh.py --pc`
Expected: `ok: telnetd under qemu`; all others exit 0; `git status` shows only the files above plus rebuilt `root/` (ignored); every `root/bin/*.bin` except `edit.bin` byte-identical to before (compare against a copy made before Step 4).

- [ ] **Step 6: Board check (only the parts that keep the user's settings)**

Build and flash nothing new for the kernel (no kernel change). Upload `root/bin/edit.bin` to `/bin/edit.bin` with `tools/board.py put`, then run `python3 tools/check_basic.py --board --only editrun`.
Expected: `ok`. If the port is held by `screen`, stop and ask.

- [ ] **Step 7: Commit**

```bash
git add userland/edit.c host_linux.c README.md tests/telnetd/ctl.c tools/check_telnetd.py
git commit -m "edit: redraw when the console size changes; host_linux: conctl reaches telnet"
```

---

### Task 2: Android project and the Telnet decoder

**Files:**
- Create: `android/settings.gradle.kts`, `android/build.gradle.kts`, `android/gradle.properties`, `android/app/build.gradle.kts`, `android/app/src/main/AndroidManifest.xml` (INTERNET permission, one launcher activity placeholder `MainActivity`), Gradle wrapper (`gradlew`, `gradle/wrapper/*`, made with the cached 9.3.1: `~/.gradle/wrapper/dists/gradle-9.3.1-all/*/gradle-9.3.1/bin/gradle wrapper --gradle-version 9.3.1`)
- Create: `android/app/src/main/java/org/t2dos/term/Telnet.kt`
- Create: `android/.gitignore` (`build/`, `.gradle/`, `local.properties`, `*.apk` outside `app/build`)
- Test: `android/app/src/test/java/org/t2dos/term/TelnetTest.kt`

**Interfaces:**
- Produces:
  - `class Telnet(private val reply: (ByteArray) -> Unit)`
  - `fun receive(buf: ByteArray, off: Int, len: Int): ByteArray` returns the data bytes; replies go to `reply`.
  - `fun encode(data: ByteArray): ByteArray` doubles IAC, CR becomes CR NUL.
  - `fun setSize(cols: Int, rows: Int)` stores the size and, if NAWS was agreed, sends `IAC SB 31 w-hi w-lo h-hi h-lo IAC SE` (any 255 byte in the size doubled).
  - `val echoByServer: Boolean`

- [ ] **Step 1: Write the failing tests** (`TelnetTest.kt`, JUnit 4):
  - `hello`: receiving `IAC WILL 1, IAC WILL 3, IAC DO 31` replies `IAC DO 1`, `IAC DO 3`, `IAC WILL 31` then NAWS with the size set before (80x24 default), and returns no data.
  - `refusesOthers`: `IAC DO 24` replies `IAC WONT 24`; `IAC WILL 24` replies `IAC DONT 24`.
  - `iacDoubled`: data `IAC IAC` gives one 255; `encode(byteArrayOf(255.toByte(), 13))` is `255 255 13 0`.
  - `splitAcrossReads`: `IAC` in one call, `WILL 1` in the next, then `IAC` / `IAC` split: same replies and data as in one read.
  - `nawsOnResize`: after the hello, `setSize(100, 30)` sends `IAC SB 31 0 100 0 30 IAC SE`; `setSize(255, 30)` sends the 255 doubled.
  - `subnegotiationSkipped`: `IAC SB 24 1 IAC SE` then `hi` gives data `hi`.

- [ ] **Step 2: Run to see them fail**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest`
Expected: compile errors / FAIL (no `Telnet`).

- [ ] **Step 3: Implement `Telnet`** as a byte state machine (DATA, IAC, OPT(cmd), SB, SB_IAC) kept across calls.

- [ ] **Step 4: Run the tests**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest`
Expected: BUILD SUCCESSFUL, all TelnetTest cases pass.

- [ ] **Step 5: Commit**

```bash
git add android
git commit -m "android: project and telnet decoder"
```

---

### Task 3: Screen

**Files:**
- Create: `android/app/src/main/java/org/t2dos/term/Screen.kt`
- Test: `android/app/src/test/java/org/t2dos/term/ScreenTest.kt`

**Interfaces:**
- Produces:
  - `class Screen(cols: Int, rows: Int, val reply: (ByteArray) -> Unit)`
  - `fun feed(data: ByteArray, off: Int = 0, len: Int = data.size)`
  - `fun resize(cols: Int, rows: Int)`
  - `val cols: Int; val rows: Int; val cursorRow: Int; val cursorCol: Int; val cursorVisible: Boolean; val altScreen: Boolean`
  - `fun cell(row: Int, col: Int): Cell` where `data class Cell(val ch: Char, val fg: Int, val bg: Int, val dim: Boolean, val reverse: Boolean)`; fg/bg 0..7, default fg 7 bg 0.
  - `fun rowText(row: Int): String` (trailing spaces trimmed; for tests)
  - `val scrollback: Int` (lines kept, max 1000) and `fun historyRowText(i: Int): String` / `fun historyCell(i: Int, col: Int): Cell` (i = 0 is the newest line that left the top)
  - `var version: Long` incremented on every change (the view redraws when it moves)

- [ ] **Step 1: Write the failing tests** (`ScreenTest.kt`), each feeding bytes and checking cells/cursor:
  - `printAndNewline`: `"ab\r\ncd"` gives rows `ab`, `cd`, cursor (1,2).
  - `backspaceTab`: `"abc\b\bX"` gives `aXc`; `"a\tb"` puts `b` at column 8.
  - `clearHome`: `ESC[2J ESC[H` clears and homes; `ESC[5;10H` puts the cursor at (4,9); `ESC[999;999H` clamps to the last cell.
  - `eraseLine`: `ESC[K` clears from the cursor to the end of the row only.
  - `attributes`: `ESC[31;44m` sets fg 1 bg 4; `ESC[7m` reverse; `ESC[2m` dim; `ESC[0m` and `ESC[m` reset.
  - `cursorVisibility`: `ESC[?25l` / `ESC[?25h`.
  - `altScreen`: text on main, `ESC[?1049h` gives a blank screen, writes there, `ESC[?1049l` shows the main text and cursor again.
  - `cursorReport`: at (4,9) `ESC[6n` makes `reply` get `ESC[5;10R`.
  - `pendingWrap`: on a 10-wide screen `"0123456789\r\nx"` gives rows `0123456789`, `x` (no blank row); `"0123456789y"` wraps `y` to the next row.
  - `scrollAndHistory`: on a 3-row screen, 5 lines leave 2 lines in history (newest first) and the screen shows the last 3; history is not kept on the alternate screen.
  - `latin1`: byte 0xE5 shows `å`; byte 0x85 shows `·`.
  - `unknownSequence`: `ESC[12;34X` and `ESC]0;title BEL` draw nothing.
  - `splitSequence`: `ESC` then `[31m` then `A` in three feeds: `A` with fg 1.
  - `resizeKeepsTopLeft`: resizing 10x3 to 5x2 keeps the top-left text and clamps the cursor.
  - `bigBurst`: 200 KB of mixed text and escape codes through `feed` in 4 KB pieces takes under 1 s.

- [ ] **Step 2: Run to see them fail**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest --tests '*ScreenTest*'`
Expected: FAIL (no `Screen`).

- [ ] **Step 3: Implement `Screen`**: two cell grids (main, alt) as arrays, a ring of up to 1000 history rows for main, a CSI parser (params, `?` prefix, final byte) and a separate state for OSC (skip to BEL or `ESC \`). Unknown finals do nothing.

- [ ] **Step 4: Run the tests**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add android/app/src/main/java/org/t2dos/term/Screen.kt android/app/src/test/java/org/t2dos/term/ScreenTest.kt
git commit -m "android: screen emulator for the codes T2DOS sends"
```

---

### Task 4: Keys and Session

**Files:**
- Create: `android/app/src/main/java/org/t2dos/term/Keys.kt`, `android/app/src/main/java/org/t2dos/term/Session.kt`
- Test: `android/app/src/test/java/org/t2dos/term/KeysTest.kt`, `android/app/src/test/java/org/t2dos/term/SessionTest.kt`

**Interfaces:**
- Consumes: `Telnet`, `Screen` (Tasks 2, 3).
- Produces:
  - `enum class Key { UP, DOWN, LEFT, RIGHT, HOME, END, PGUP, PGDN, DEL, ESC, TAB, ENTER, BACKSPACE }`
  - `object Keys { fun key(k: Key): ByteArray; fun text(s: String, ctrl: Boolean): ByteArray }`. `text` maps each char: with `ctrl`, letters a-z/A-Z become 1..26 (other chars unchanged); 32..126 and 160..255 one byte; `\n` becomes CR; anything else is dropped.
  - `class Session(host: String, port: Int, password: String?, cols: Int, rows: Int, listener: Listener)` with `fun start()`, `fun send(bytes: ByteArray)`, `fun resize(cols: Int, rows: Int)`, `fun close()`, `val screen: Screen`.
  - `interface Listener { fun changed(); fun closed(reason: String) }` (called on the session thread; the UI posts to its own thread).
  - Login rule: once per connection, when the screen's text ends with `password: ` and a password is set, send it plus CR; never again on that connection.
  - `closed(reason)`: the last bracketed line the board sent (`[logged out]`, `[another login took over this console]`, `[telnetd stopped]`), else `try again later` / `another login is in progress` if seen, else the socket error's message, else `connection closed`.

- [ ] **Step 1: Write the failing tests**
  - `KeysTest`: every row of the Global Constraints key list; `text("a", ctrl = true)` is `[1]`; `text("å", false)` is `[0xE5]`; `text("€", false)` is empty; `text("\n", false)` is `[13]`.
  - `SessionTest` with a local `ServerSocket` playing the board:
    - `autoLoginOnce`: server sends the hello and `T2DOS `, then 300 ms later `password: `; the session answers with the hello replies and `secret\r\0`; server sends `\r\nwrong password\r\npassword: `; the session sends nothing more within 1 s.
    - `closeReason`: server sends `\r\n[logged out]\r\n` and closes: `closed("[logged out]")` within 1 s.
    - `dropMidOutput`: server sends 10 KB then resets the connection: `closed` is called once, `screen` holds the text received.
    - `typedBytesEncoded`: `send(Keys.key(Key.ENTER))` reaches the server as `13 0`.

- [ ] **Step 2: Run to see them fail**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest`
Expected: FAIL (no `Keys`, `Session`).

- [ ] **Step 3: Implement `Keys` and `Session`**: one reader thread (blocking `read` into 4 KB, `Telnet.receive`, `Screen.feed` under a lock, `changed()`); writes go through `Telnet.encode` on a single-thread executor so the UI never blocks on the socket; connect timeout 5 s; `TCP_NODELAY` on.

- [ ] **Step 4: Run the tests**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add android/app/src/main/java/org/t2dos/term/Keys.kt android/app/src/main/java/org/t2dos/term/Session.kt android/app/src/test
git commit -m "android: keys and the telnet session"
```

---

### Task 5: End-to-end against the real shell under qemu

**Files:**
- Create: `android/app/src/test/java/org/t2dos/term/BoardE2ETest.kt` (skipped with `Assume` unless `T2DOS_E2E_PORT` is set)
- Create: `tools/check_android.py`

**Interfaces:**
- Consumes: `Session`, `Keys`, `Screen` (Tasks 2-4); Task 1's board side.
- Produces: `python3 tools/check_android.py` prints `ok: android` and exits 0 when the Android unit tests and the end-to-end test pass.

- [ ] **Step 1: Write `tools/check_android.py` and the failing test.** The script builds (`make`, `make host`), starts `qemu-arm ./host_linux -r root /bin/sh.bin` like `check_telnetd.py`'s `PcConsole`, types `telnetd on PORT` with password `sesame` (twice), copies `/lib/mtab.c` to `/tmp/e2e.txt` through the shell, then runs `./gradlew --offline :app:testDebugUnitTest` with `T2DOS_E2E_PORT=PORT` and `T2DOS_E2E_PASS=sesame` in the environment, then types `telnetd off` and ends qemu. `BoardE2ETest` (80x24 session, auto-login):
  - `shell`: after `Press Enter for a prompt.`, Enter gives a row ending `/> `; `ls /bin` shows `edit.bin` on screen.
  - `catBig`: `cat /tmp/e2e.txt` ends with the file's last line on the row above the prompt.
  - `editTypeSaveQuit`: `edit /tmp/e2e2.txt`; the alternate screen is on and the last row is edit's status line; type `hello`, `^S`, `^X`; the main screen is back with the prompt; `cat /tmp/e2e2.txt` shows `hello`.
  - `editResize`: in `edit /tmp/e2e2.txt`, `session.resize(60, 20)`; within 1 s the status line is on row 19 and `screen.rows == 20`; quit with `^X`.

- [ ] **Step 2: Run it**

Run: `python3 tools/check_android.py`
Expected: the new tests run (not skipped). If one fails, fix the app code, not the test.

- [ ] **Step 3: Run until it passes**

Run: `python3 tools/check_android.py`
Expected: `ok: android`.

- [ ] **Step 4: Commit**

```bash
git add tools/check_android.py android/app/src/test/java/org/t2dos/term/BoardE2ETest.kt
git commit -m "android: end-to-end test against the shell and edit under qemu"
```

---

### Task 6: The app: terminal view, extra keys, boards list

**Files:**
- Create: `android/app/src/main/java/org/t2dos/term/TerminalView.kt`, `TerminalActivity.kt`, `MainActivity.kt` (boards list), `BoardStore.kt`, `Secret.kt` (Keystore AES-GCM), `Layout.kt` (pure size maths), layouts under `android/app/src/main/res/`, launcher icon (simple vector)
- Test: `android/app/src/test/java/org/t2dos/term/LayoutTest.kt`, `BoardStoreTest.kt`

**Interfaces:**
- Consumes: `Session`, `Screen`, `Keys`, `Key`.
- Produces:
  - `object Layout { fun grid(widthPx: Int, heightPx: Int, cellW: Float, cellH: Float): Pair<Int, Int> }` (cols, rows; at least 20x4, at most 1024 each).
  - `data class Board(val id: Long, val name: String, val host: String, val port: Int, val fontSp: Float, val secret: String?)` (`secret` is the encrypted password, Base64, or null).
  - `object BoardStore { fun encode(list: List<Board>): String; fun decode(s: String): List<Board> }` (one board per line, fields tab-separated, tab/newline/backslash escaped as `\t` `\n` `\\`; stored in SharedPreferences; no `org.json`, whose Android stubs throw in JVM tests).
  - `object Secret { fun seal(plain: String): String; fun open(sealed: String): String? }` (Keystore key alias `t2dos-board-pw`, AES/GCM/NoPadding).

- [ ] **Step 1: Write the failing tests**
  - `LayoutTest`: 1080x1500 px with 27x54 px cells gives (40, 27); a tiny view gives (20, 4); huge gives at most 1024.
  - `BoardStoreTest`: encode then decode keeps every field, a null secret and a name with quotes and `å`; decoding `""` gives an empty list.

- [ ] **Step 2: Run to see them fail**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest`
Expected: FAIL (no `Layout`, `BoardStore`).

- [ ] **Step 3: Implement `Layout`, `BoardStore`, `Secret`.**

- [ ] **Step 4: Implement the UI**
  - `TerminalView`: monospace `Paint`, cell size from `measureText("M")` and font metrics; draws `Screen` cells with the 8 ANSI colours (dark scheme, fg 7 light grey, dim at half alpha, reverse swaps) and a block cursor when visible; on size change calls `Layout.grid` then `session.resize`; pinch (`ScaleGestureDetector`) changes `fontSp` (8..32 sp) and saves it to the board; vertical drag scrolls back through history when not on the alternate screen, any output or key returns to the bottom; tap shows the keyboard; long-press pastes the clipboard through `Keys.text`.
  - Keyboard: `onCheckIsTextEditor() = true`; `onCreateInputConnection` sets `inputType = TYPE_NULL` and `imeOptions = IME_FLAG_NO_EXTRACT_UI or IME_FLAG_NO_FULLSCREEN`, returns a `BaseInputConnection(this, false)` subclass whose `commitText` sends `Keys.text(text, ctrlSticky)`, `setComposingText` sends nothing and `finishComposingText` sends the composed text once, `deleteSurroundingText(n, 0)` sends n x BACKSPACE, `sendKeyEvent` maps `KEYCODE_DEL`, `KEYCODE_ENTER`, `KEYCODE_FORWARD_DEL`, D-pad, `KEYCODE_MOVE_HOME/END`, `KEYCODE_PAGE_UP/DOWN`, `KEYCODE_ESCAPE`, `KEYCODE_TAB` to `Keys.key`, and Ctrl+letter from a hardware keyboard to 1..26. `onKeyDown` does the same for hardware keys.
  - Extra-keys row (`HorizontalScrollView` of buttons): Esc, Ctrl (sticky for the next key, double tap locks, shown highlighted), Tab, ←, ↑, ↓, →, Home, End, PgUp, PgDn, Del, ^C.
  - `TerminalActivity`: takes a board id; asks for the password in a masked dialog when the board has none saved (with a "remember" checkbox that saves it through `Secret.seal`); starts `Session`; `FLAG_KEEP_SCREEN_ON` while connected; on `closed(reason)` shows a bar "Disconnected: reason" with a Reconnect button and keeps the screen; `windowSoftInputMode = adjustResize` so the keyboard shrinks the terminal (the board then gets the new size by NAWS).
  - `MainActivity`: the boards list (name and host:port), + to add, tap to connect, long-press to edit or delete, an edit form with name, host, port (default 23), password (optional, saved sealed) and "forget password".

- [ ] **Step 5: Build and run the tests**

Run: `cd android && ./gradlew --offline :app:testDebugUnitTest :app:assembleDebug && python3 ../tools/check_android.py`
Expected: BUILD SUCCESSFUL, `android/app/build/outputs/apk/debug/app-debug.apk` exists, `ok: android`.

- [ ] **Step 6: Commit**

```bash
git add android
git commit -m "android: terminal view, extra keys and boards list"
```

---

### Task 7: README and phone install

**Files:**
- Modify: `README.md` (telnetd section: a short "T2DOS Terminal (Android)" part: build with `cd android && ./gradlew assembleDebug`, install with `adb install -r app/build/outputs/apk/debug/app-debug.apk`, add the board, the extra keys, Latin-1 note, `edit` follows the size)

- [ ] **Step 1: Write the README part.**
- [ ] **Step 2: If a phone shows in `adb devices`, install it with `adb install -r`.** Otherwise leave the APK path for the user.
- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "README: the Android terminal"
```
