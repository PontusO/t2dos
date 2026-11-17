# T2DOS Android terminal: design

## Why

The board's telnetd (`kernel/telnetd.c`, README "telnetd") lets a phone use the board's console
over WiFi. The user tried several Android telnet/SSH apps; the ones that work well want a
subscription for something this simple. They want their own free app, made for this board,
that works fully with it: the shell, `edit`, BASIC programs, and full-screen programs written
later.

## Goals

- Connect to a T2DOS board's telnetd, log in, and use the console as on USB.
- Full-screen programs work: `edit` must be fully usable (open, move, type, cut, save, run with
  Ctrl-R, quit), and so must anything drawn with the kernel's `conctl()` codes.
- Every key the board's programs use can be typed on a phone: Esc, Ctrl+letter, Tab, arrows,
  Home/End, PgUp/PgDn, Del.
- A short list of saved boards (name, host, port), one tap to connect.
- Built here, installed on the user's phone with `adb`; no Play Store, no account, no ads.

## Non-goals

- A general terminal for other hosts (SSH, xterm compatibility, Unicode width tables, mouse).
- Play Store publishing, translations, tablets-first layouts, themes beyond one dark scheme.
- Several sessions at once (the board takes one telnet session at a time anyway).

## Anti-goals (would make it a failure even if it runs)

- `edit` misdraws or a key does nothing: the app is then no better than the free apps tried.
- Dependence on outside libraries or services beyond the Android SDK/AndroidX basics.
- A password stored in plain text.

## Constraints

- Board side, fixed (do not change telnetd for the app): on connect the board sends
  `IAC WILL ECHO, IAC WILL SGA, IAC DO NAWS`, then `T2DOS password: `. It refuses other options
  with DONT/WONT. It sends output with LF as CR LF and IAC doubled. A wrong password costs 2 s,
  three in a row lock logins 5 s. A login that takes over prints
  `[another login took over this console]` to the old session; `exit` prints `[logged out]`;
  `telnetd off` prints `[telnetd stopped]`; then the board closes the connection.
- The board accepts NAWS sizes 1..1024 and uses them as the console width/height (`edit` and
  `sysinfo(INFO_CONSOLE)`). It sends no other negotiation later, but NAWS updates are taken any
  time.
- Toolchain on this PC: Android SDK in `~/Android/Sdk` (platforms 34-36, build-tools up to
  36.0.0), Java 21, `adb`, a cached Gradle 9.3.1. No Android Studio is required.

### What the board sends (the screen codes the app must handle)

From `kernel/kernel.ino` `SerialConsole::ansi` and `userland/edit.c`:

| Bytes | Meaning |
|---|---|
| CR, LF, BS (8), TAB | usual meanings; LF moves down (scrolls at the bottom) |
| `ESC[2J`, `ESC[H` | clear screen, cursor home |
| `ESC[r;cH` | cursor to row r, column c (1-based, clamped; `ESC[999;999H` is used) |
| `ESC[K` | clear to end of line |
| `ESC[0m`, `ESC[2m`, `ESC[7m`, `ESC[30..37;40..47m` | attributes: reset, dim, reverse, colours |
| `ESC[?25l` / `ESC[?25h` | hide / show cursor |
| `ESC[?1049h` / `ESC[?1049l` | alternate screen on / off (edit's screen; back to the shell's text after) |
| `ESC[6n` | cursor position request: the app must answer `ESC[row;colR` (edit's `term_size`) |

Bytes 128-255 are sent as is (a file's bytes). The app shows every byte as one Latin-1
character in one cell (user's choice: it keeps the screen in step with `edit`, which counts one
byte as one column); 128-159 show as a placeholder glyph. A UTF-8 file from the PC therefore
shows its å as two odd characters; that is accepted. Unknown escape sequences are swallowed without drawing. Future programs may
add codes; the emulator is written so a new one is one added case.

### What the app sends (keys)

Matching `userland/edit.c` `getkey` and `userland/sh.c`: arrows `ESC[A/B/C/D`, Home `ESC[H`,
End `ESC[F`, PgUp `ESC[5~`, PgDn `ESC[6~`, Del `ESC[3~`, Esc 27, Tab 9, Backspace 127, Enter
CR (sent as CR NUL, per telnet), Ctrl+letter 1..26. Ctrl-C is a plain byte 3 (the board treats
it as break). Typed characters 160-255 (å, ä, ö, é, ...) are sent as one Latin-1 byte; other
non-ASCII characters are not sent.

## Approaches considered

- **A. Own small terminal in Kotlin (chosen by the user).** Native app, no outside libraries.
  Draws the screen itself and understands exactly the codes above; telnet handshake; extra-keys
  row; saved boards. Small, testable on the PC; a future code needs one small app change, and
  the user owns both ends.
- **B. Termux's terminal-emulator/terminal-view libraries.** Full xterm emulation, scrollback
  and selection for free, but a large outside dependency from a third-party package host, built
  around a local pty, harder to adjust. Not chosen.
- Ruled out: a web page (browsers cannot open raw TCP), Flutter (not installed, no gain).

## Design

### Board-side changes (both decided by the user or needed for testing)

- **`edit` follows size changes** (user's choice). On a phone the size changes often (keyboard
  shown or hidden, rotation), and the app sends each change by NAWS. `edit` today reads the size
  once at start (`term_size()`). It will check `sysinfo(INFO_CONSOLE)` before each key wait (and
  while waiting, so a change with no key press redraws too) and, when width or height changed,
  take the new size, keep the cursor in view and redraw the whole screen. When the console gives
  no size (USB without a telnet session, where `INFO_CONSOLE` stays 80x24), `edit` keeps its
  start-up `ESC[6n` measurement as today. Screen programs written later follow the same pattern;
  README's `edit` text says so. Tested with check_basic/edit cases where they apply and with the
  PC test below (resize during `edit`).
- **`host_linux` forwards `conctl()`** to the telnet session the way the board's
  `SharedConsole::ctl` does (today `SYS_conctl` returns 0 there), so the PC test sees clear,
  cursor and colour codes from the real kernel path. My call.

### Where it lives

`android/` in this repo (the app belongs to the project and versions with telnetd), a plain
Gradle project with the Gradle wrapper. Package id `org.t2dos.term`, app name "T2DOS Terminal".
Kotlin, minSdk 26, target/compile 36. AndroidX core/appcompat only if needed; no other
libraries.

### Parts

1. **Telnet** (plain Kotlin, no Android types): turns the socket's byte stream into data bytes
   and replies. Answers `WILL ECHO` with `DO ECHO`, `WILL SGA` with `DO SGA`, `DO NAWS` with
   `WILL NAWS` then the window size; refuses anything else; un-doubles IAC; drops other
   commands. Outgoing: doubles IAC, CR becomes CR NUL, sends NAWS again on every size change.
2. **Screen** (plain Kotlin): a grid of cells (char, fg, bg, dim, reverse), cursor, cursor
   visibility, main and alternate screen, a scrollback of 1000 lines for the main screen only.
   Fed data bytes; produces replies (`ESC[6n`). Resizes keep the text top-left and clamp the
   cursor. Line wrap at the right edge as a VT100 does (pending wrap, so a full-width line
   followed by CR LF makes one line, not two).
3. **TerminalView** (Android View): draws the grid with a monospace font, the cursor as a
   block; picks columns and rows from the view size and font size; pinch changes the font size
   (kept per board); swipe up/down scrolls back through the main screen's history (any output
   or key returns to the bottom). Opens the soft keyboard on tap and reads it as raw keys
   (`onCreateInputConnection` returning a plain input type with no suggestions/autocorrect).
   Long-press pastes the clipboard. A hardware keyboard works too.
4. **Extra-keys row** above the keyboard: Esc, Ctrl (sticky for the next key; double tap locks),
   Tab, arrows, Home, End, PgUp, PgDn, Del, and `^C`. Fixed layout, scrolls sideways if it does
   not fit.
5. **Boards list** (first screen): name, host, port (default 23), optional saved password.
   Add, edit, delete, tap to connect. A saved password is encrypted with an Android Keystore
   key; without one the app asks at each connect (masked field). The app types the password
   itself when it sees `password: ` (the board sends `T2DOS ` and `password: ` separately,
   and re-prompts with plain `password: `). It does this once per connection: after a
   `wrong password` it stops and shows the prompt to the user, so it can never walk into the
   board's 3-try lock.
6. **Session**: a foreground connection on a background thread. Keeps the screen on while
   connected. When the board closes the connection, the last screen stays visible with a bar
   "Disconnected: <reason>" and a Reconnect button. No automatic reconnect (a takeover by
   another login must not fight back).

### Look

```
+------------------------------------------+
| /> ls                                    |
|    <dir>  bin/                           |
|    ...                                   |
| /> _                                     |
|                                          |
+------------------------------------------+
| Esc Ctrl Tab  ←  ↑  ↓  →  Home End PgUp >|
+------------------------------------------+
|            (Android keyboard)            |
+------------------------------------------+
```

Dark background, light grey text, the 8 ANSI colours. Portrait and landscape both work; the
size change goes to the board by NAWS.

### Testing

- JVM unit tests for Telnet and Screen (no device): every row of both tables above, wrap,
  scroll, alternate screen, resize, split escape sequences across reads.
- A PC test that runs the real board software: start `qemu-arm ./host_linux -r root
  /bin/sh.bin` (as `tools/check_telnetd.py --pc` does), turn telnetd on, connect with the app's
  Telnet+Screen classes, log in, run `ls`, open `edit` on a file, type, save, quit, and check
  the screen grid and the saved file; then change the size by NAWS while `edit` is open and
  check that it redraws at the new size. Wire it as `tools/check_android.py` or a Gradle test that
  check_android calls.
- On the phone: install with `adb install`, then the user tries the shell and `edit` on the
  real board.

## Left to the builder

Exact class and file names, Gradle/AGP versions that work with the cached Gradle 9.3.1 and
SDK 36, colours, font, icon, the boards list layout, how the extra-keys row looks, how the
Keystore encryption is done.

## Calls I made (for the user to check)

- The app lives in `android/` in the t2dos repo.
- Saved passwords are optional and encrypted with the Android Keystore.
- 1000 lines of scrollback, main screen only; pinch for font size; long-press to paste; no
  text selection/copy in the first version.
- No automatic reconnect.
- Package id `org.t2dos.term`, name "T2DOS Terminal".
- `host_linux` forwards `conctl()` to the telnet session, so the PC test covers those codes.
- Auto-login tries the saved password once per connection.
