# NOTES: file mtimes (round 2026-11-22 round B) — DONE

Goal: stamp every card write from the wall clock (`SDFS.setTimeCallback`
+ kernel `fs_time_cb`), `ls` shows local mtimes DST-correct per file
(`SYS_tzlocal = 108` in all four places, `----------` outside the known
window), BASIC `DATE$`/`TIME$`. Spec:
`docs/superpowers/specs/2026-11-22-mtime-roundb-design.md` (Results
there too), plan `docs/superpowers/plans/2026-11-22-mtimes.md`. SDD
ledger (all rulings): `.superpowers/sdd/2026-11-22-mtimes/progress.md`,
board evidence trail in `task-5-report.md` beside it. On branch
`mtimes`, awaiting merge.

## Settled (don't re-investigate)

- The DOS-seconds round-trip trap: SDFS's `dateTimeCB` writes *raw*
  `tm_sec` into the 5-bit DOS field, while `FatToTimeT` reads it back
  spec-compliant as `sec * 2` (seconds >= 32 carry into the minute), so
  only second 0 round-trips - a verbatim 23:08:45 reads back 23:09:26.
  `fs_time_cb` truncating to whole minutes is what makes
  `getLastWrite()`/`fileTime()` exact; keep it. PC tests cannot see
  this (the FAT path is board-only); the board minute-exact leg is the
  witness.
- The DOS fields hold the *UTC* calendar because SDFS converts via
  `localtime_r`, which on this libc with no `TZ` machinery *is* UTC.
  Storage truth stays UTC (tz-round contract); all localization
  happens at display time through `tzlocal`.
- Both clamp bounds are principled, not an arbitrary year:
  `mtime <= MTIME_UNKNOWN` (315532800, 1980-01-01) is at-or-below the
  FAT epoch floor - unrepresentable, and the sentinel unsynced writes
  store (it arrives via readdir as 315532800, not 0; a real file can
  only carry it in the first minute of 1980, i.e. never).
  `mtime >= MTIME_MAX` (2147483647) is past the int32 epoch ceiling the
  whole OS lives under: cards already hold junk ~2097 stamps that
  pre-stamping SDFS wrapped from `time(nullptr) = 0`. Net effect: old
  pre-Round-B files show `----------` until rewritten - truthful, their
  stamps are junk. Both bounds are exclusive (pinned by
  `sentinel.bin`/`ceiling.bin` in the `ls_times` case).
- Stale-`dirent_t` hazard: the struct grew from 72 to 76 bytes; a card
  binary still allocating the old size takes a 4-byte kernel write past
  its buffer after this kernel. The authoritative re-`put` rule is
  `grep -l 'readdir\|dirent_t' src/*/*.c` - this round {basic ls rcp
  sh}; `rls` does NOT count (net `LS` protocol, never the syscall). No
  compat syscall: one kernel+bin set, accepted (`stat_t` precedent).
- `tzlocal(instant)` vs `tzset(0)`: `tzlocal(utc)` answers the offset
  *of that instant* under the active rule; `tzset(0)` answers *now*.
  Never render a past mtime with today's offset (`ls` exists as the
  counterexample-fixed case: a July file lists with its +02:00 side in
  November).
- BASIC reserves `DATE$` and `TIME$` as zero-arg keywords (the INKEY$/
  TIMER `fn_tab`+`fnop` pattern): they cannot be used as variables and
  assigning to them is a syntax error. Intended, do not "fix".
- Persistent gates must be season-independent AND ambient-TZ-proof.
  Two seasonal landmines from the plan were found and fixed this round
  (tz.c's winter-only `== 3600` leg -> year-round 3600-or-7200; the
  `ls_times` golden's tz-echo -> explicit `TZ=` in `ls_times.env`,
  golden ls-only). `check_sh` now honors per-case `NAME.env` files like
  `check_basic` (applied *after* the harness TZ strip), and an explicit
  case `TZ` is the ONLY sanctioned way to set a zone in a gate - the
  harness strip stays, and mtimes in goldens are staged absolutely.
- `src/basic/io.c:430` (`fst.size > 0 || fst.mtime > 0`) is settled:
  `f_path` is only ever set for `/pipe/` paths (`io.c:395-397`) and a
  pipe stat returns `mtime = nbounds`, so no regular file reaches that
  escape and stamping cannot change it. No change; nobody re-opens it.
- Board quirks for future probe scripts (task-5-report.md): board `sh`
  has no `;` chaining or pipes (multi-command one-liners misparse -
  send single commands); the post-boot unsynced window is only ~9-14 s,
  so legs that race it need a single-session pyserial script (the
  reusable pattern: retry-open through the reboot re-enumeration); the
  board cannot reach 127.0.0.1, use the PC's LAN IP; the user runs a
  long-lived `tools/fileserver.py` on 7777 - never kill it, use another
  port.

## Gates at completion

`make check` ok (12 toolchain cases, 7 error cases, symbols, archive,
driver); `check_ref` different: 0 with `tzlocal` in the newer-builtins
exclusion; `check_toolchain --pc` 12 cases (the `tz` case gained the
`tzlocal` fixed-epoch legs, TZ stripped from the subprocess env, the
case sets its own explicitly); `check_sh --pc` 6 cases (new `ls_times`:
sentinel + ceiling pins, DST-sides rendered per file, sorted compare);
`check_basic --pc` 140 cases ASan-clean; `check_net --pc` ok;
`bench --pc` ok. Board (task-5-report.md, Challenger 2350 at
`6aba4ab`): kernel 262428 bytes flash (25%), RAM unchanged 164884;
minute-exact synced stamping winter `+01:00`; `get` stamped at
transfer; junk stamps `cat.bin`/`wc.bin`, ramfs and mounted `/mnt/fs`
all `----------`; unsynced-boot write won the ~10 s window by 0.4 s on
attempt 3; zone and mtimes survive reboot; 10/10 `ls /bin` round-trips
0.57 s; `--selfhost` six IDENTICAL attempt 1.
