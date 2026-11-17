// telnetd.c - the remote console core: telnet sessions that share the console with USB.
//
// At most one session is logged in (the live one); one more may be logging in meanwhile, and
// when it logs in it takes over from the live one (a phone that dropped off the WiFi leaves a
// connection behind that may take minutes to time out).  Wrong passwords count for every
// connection together: after one, the next try (on any connection) waits 2 s, and after three in
// a row nobody may log in for 5 s.  A connection has 60 s to log in.  Sending to a connection that
// is not logged in never waits: one that does not take what it is sent is closed.
//
// The server asks the client to let it echo (WILL ECHO) and to send each key as it is typed
// (WILL SUPPRESS-GO-AHEAD), and for the window size (DO NAWS).  Input: telnet commands are
// removed, CR LF and CR NUL become CR, IAC IP and IAC BRK become Ctrl-C.  Output: LF becomes
// CR LF and 255 is doubled.
#include <string.h>
#include <stdio.h>
#include "telnetd.h"

enum { S_FREE, S_LOGIN, S_WAIT, S_LIVE };
enum { T_DATA, T_CR, T_IAC, T_OPT, T_SB, T_SBIAC };
enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, BRK = 243, IP = 244, SE = 240,
       O_ECHO = 1, O_SGA = 3, O_NAWS = 31 };
#define LOGIN_MS 60000
#define RETRY_MS 2000
#define LOCK_MS 5000
#define TRIES 3              // wrong passwords in a row before the lock

struct sess {
  int state, h, bad;               // bad: a send failed, close it
  unsigned t0, until;              // accepted at; S_WAIT until
  int ts, cmd;                     // the telnet filter's state, and the WILL/WONT/DO/DONT seen
  unsigned char sb[8]; int sbn;    // a subnegotiation
  int w, ht;                       // NAWS
  char pw[TD_PASSMAX + 2]; int pn;
  char ip[48];
};
static struct sess live, login;    // live.state is S_LIVE or S_FREE
static char pass[TD_PASSMAX + 1];
static int on;
static unsigned lock_until; static int locked;   // after three wrong passwords
static unsigned next_try; static int fails;       // after a wrong one; wrong ones in a row
static unsigned char in[128]; static int in_head, in_n;
static unsigned char out[256]; static int out_n;

static int after(unsigned now, unsigned t) { return (int)(now - t) >= 0; }
static void send_to(struct sess *s, const void *b, int n) {
  if (s->state != S_LIVE && td_room(s->h) < n) s->bad = 1;
  else if (td_send(s->h, b, n) < 0) s->bad = 1;
}
static void say(struct sess *s, const char *m) { send_to(s, m, strlen(m)); }
static void note(const char *what, struct sess *s) { char m[96]; snprintf(m, sizeof m, "telnetd: %s %s", s->ip, what); td_note(m); }
static void drop(struct sess *s) { td_close(s->h); s->state = S_FREE; memset(s->pw, 0, sizeof s->pw); s->pn = 0; }

// one byte from the connection: the data byte it gives, or -1
static int filt(struct sess *s, int c) {
  switch (s->ts) {
  case T_CR: s->ts = T_DATA; if (c == 10 || c == 0) return -1;   // fall through: c is data
  case T_DATA:
    if (c == IAC) { s->ts = T_IAC; return -1; }
    if (c == 13) s->ts = T_CR;
    return c;
  case T_IAC:
    s->ts = T_DATA;
    if (c == IAC) return IAC;
    if (c >= WILL && c <= DONT) { s->cmd = c; s->ts = T_OPT; }
    else if (c == SB) { s->sbn = 0; s->ts = T_SB; }
    else if (c == IP || c == BRK) return 3;
    return -1;
  case T_OPT: {                    // refuse what we did not offer; the client never asks twice
    unsigned char r[3] = { IAC, 0, (unsigned char)c };
    s->ts = T_DATA;
    if (s->cmd == WILL && c != O_NAWS) { r[1] = DONT; send_to(s, r, 3); }
    if (s->cmd == DO && c != O_ECHO && c != O_SGA) { r[1] = WONT; send_to(s, r, 3); }
    return -1; }
  case T_SB:
    if (c == IAC) s->ts = T_SBIAC;
    else if (s->sbn < (int)sizeof s->sb) s->sb[s->sbn++] = c;
    return -1;
  case T_SBIAC:
    if (c == IAC) { if (s->sbn < (int)sizeof s->sb) s->sb[s->sbn++] = c; s->ts = T_SB; return -1; }
    s->ts = T_DATA;
    if (c == SE && s->sbn == 5 && s->sb[0] == O_NAWS) {
      int w = s->sb[1] << 8 | s->sb[2], ht = s->sb[3] << 8 | s->sb[4];
      if (w >= 1 && w <= 1024 && ht >= 1 && ht <= 1024) { s->w = w; s->ht = ht; }   // else keep the last
    }
    return -1;
  }
  return -1;
}

static int same(const char *a, int na, const char *b) {   // compares every byte, whatever matches
  int nb = strlen(b), d = na ^ nb, i;
  for (i = 0; i < TD_PASSMAX; i++) d |= (i < na ? a[i] : 0) ^ (i < nb ? b[i] : 0);
  return d == 0;
}

static void prompt(struct sess *s) { say(s, "password: "); s->state = S_LOGIN; }

static void logged_in(void) {
  if (live.state == S_LIVE) { say(&live, "\r\n[another login took over this console]\r\n"); note("was taken over", &live); drop(&live); }
  fails = 0;
  live = login; live.state = S_LIVE;
  memset(&login, 0, sizeof login); login.state = S_FREE;
  memset(live.pw, 0, sizeof live.pw); live.pn = 0;
  in_n = 0; out_n = 0;
  say(&live, "\r\nT2DOS console, shared with USB.  Press Enter for a prompt.\r\n");
  note("logged in", &live);
}

static void check(void) {
  if (on && login.pn <= TD_PASSMAX && same(login.pw, login.pn, pass)) { logged_in(); return; }
  memset(login.pw, 0, sizeof login.pw); login.pn = 0;
  say(&login, "\r\nwrong password\r\n");
  next_try = td_millis() + RETRY_MS;
  if (++fails >= TRIES) {
    note("failed to log in", &login); drop(&login);
    lock_until = td_millis() + LOCK_MS; locked = 1; fails = 0;
    return;
  }
  login.state = S_WAIT; login.until = next_try;
}

static void login_input(void) {
  unsigned char b[32]; int b_h = login.h, n = td_recv(login.h, b, sizeof b), i, c;
  if (n < 0) { drop(&login); return; }
  for (i = 0; i < n && login.state != S_FREE; i++) {
    if ((c = filt(&login, b[i])) < 0 || login.state != S_LOGIN) continue;   // S_WAIT drops keys
    if (c == 13 || c == 10) {
      check();
      if (live.state == S_LIVE && login.state == S_FREE && live.h == b_h) {   // logged in: the rest is input
        for (i++; i < n; i++) if ((c = filt(&live, b[i])) >= 0 && in_n < (int)sizeof in) in[(in_head + in_n++) % sizeof in] = c;
        return;
      }
      if (login.state == S_FREE) return;
    }
    else if (c == 8 || c == 127) { if (login.pn) login.pn--; }
    else if (c == 3 || c == 4) { say(&login, "\r\n"); drop(&login); return; }
    else if (c >= 32 && login.pn < TD_PASSMAX + 1) login.pw[login.pn++] = c;
  }
}

static void live_input(void) {
  unsigned char b[64]; int room = (int)sizeof in - in_n, n, i, c;
  if (room <= 0) return;           // the program has not read the earlier keys yet
  n = td_recv(live.h, b, room < (int)sizeof b ? room : (int)sizeof b);
  if (n < 0) { note("disconnected", &live); drop(&live); in_n = 0; return; }
  for (i = 0; i < n; i++) if ((c = filt(&live, b[i])) >= 0) in[(in_head + in_n++) % sizeof in] = c;
}

void td_start(const char *p) { strncpy(pass, p, TD_PASSMAX); pass[TD_PASSMAX] = 0; on = 1; }
void td_stop(const char *why) {
  td_flush();
  if (live.state == S_LIVE) { say(&live, "\r\n["); say(&live, why); say(&live, "]\r\n"); drop(&live); }
  if (login.state != S_FREE) drop(&login);
  memset(pass, 0, sizeof pass); on = 0; in_n = 0; out_n = 0;
}

int td_logout(const char *why) {
  if (live.state != S_LIVE) return 0;
  td_flush();
  say(&live, "\r\n["); say(&live, why); say(&live, "]\r\n");
  note("logged out", &live); drop(&live); in_n = 0;
  return 1;
}

void td_poll(void) {
  unsigned now = td_millis();
  int h;
  if (!on) return;
  if (login.state != S_FREE) login_input();       // first, so a closed one frees the slot
  if (login.state != S_FREE && login.bad) drop(&login);
  while ((h = td_accept()) >= 0) {
    static const unsigned char hello[] = { IAC, WILL, O_ECHO, IAC, WILL, O_SGA, IAC, DO, O_NAWS };
    struct sess s;
    memset(&s, 0, sizeof s); s.h = h; s.t0 = now; td_peer(h, s.ip, sizeof s.ip);
    if (locked && after(now, lock_until)) locked = 0;
    if (login.state != S_FREE || locked) {
      say(&s, login.state != S_FREE ? "another login is in progress\r\n" : "try again later\r\n");
      td_close(h); continue;
    }
    login = s; login.state = S_WAIT;
    send_to(&login, hello, sizeof hello);
    say(&login, "T2DOS ");
    if (!fails || after(now, next_try)) prompt(&login); else login.until = next_try;   // next_try counts after a wrong one
    if (login.bad) drop(&login);
  }
  if (login.state != S_FREE && after(now, login.t0 + LOGIN_MS)) { say(&login, "\r\ntimed out\r\n"); drop(&login); }
  if (login.state == S_WAIT && after(now, login.until)) prompt(&login);
  if (login.state != S_FREE) login_input();
  if (login.state != S_FREE && login.bad) drop(&login);
  if (live.state == S_LIVE) live_input();
}

int td_avail(void) { return in_n; }
int td_peek(void) { return in_n ? in[in_head] : -1; }
int td_getc(void) {
  int c;
  if (!in_n) return -1;
  c = in[in_head]; in_head = (in_head + 1) % sizeof in; in_n--;
  return c;
}

void td_flush(void) {
  if (out_n && live.state == S_LIVE && td_send(live.h, out, out_n) < 0) { note("disconnected", &live); drop(&live); in_n = 0; }
  out_n = 0;
}
static void put1(int c) { if (out_n == (int)sizeof out) td_flush(); out[out_n++] = c; }
void td_putc(int c) {
  if (live.state != S_LIVE) return;
  c &= 255;
  if (c == '\n') put1('\r');
  if (c == IAC) put1(IAC);
  put1(c);
}

int td_live(void) { return live.state == S_LIVE; }
int td_size(void) { return live.state == S_LIVE && live.w > 0 && live.ht > 0 ? live.w | live.ht << 16 : 0; }
const char *td_live_ip(void) { return live.state == S_LIVE ? live.ip : ""; }
