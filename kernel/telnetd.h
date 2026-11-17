// telnetd.h - the remote console: a telnet session that shares the console with USB (see
// telnetd.c).  Plain C, used by the board kernel (kernel.ino) and by host_linux.c.
#ifndef TELNETD_H
#define TELNETD_H
#ifdef __cplusplus
extern "C" {
#endif

#define TD_PASSMAX 32      // a password is 1..TD_PASSMAX bytes

// The transport, written by the user of the core.  None of them may block for long or switch
// tasks.  td_accept returns a new connection's handle (>= 0), or -1 if none is waiting;
// td_recv returns the bytes read, 0 if none are waiting, -1 once the connection is closed;
// td_send sends all n bytes (waiting at most a few seconds) or returns -1.  td_note prints a line on the local console only.
int td_accept(void);
int td_recv(int h, void *b, int n);
int td_send(int h, const void *b, int n);
int td_room(int h);                // bytes td_send can take now without waiting
void td_close(int h);
void td_peer(int h, char *ip, int sz);
void td_note(const char *msg);
unsigned td_millis(void);

void td_start(const char *pass);   // connections may log in with pass from now on (a live session stays)
void td_stop(const char *why);     // tells the live session why, then closes every session
int td_logout(const char *why);   // ends the live session with why: 1, or 0 if none was logged in
void td_poll(void);                // accepts connections, runs the logins, reads the live session
int td_avail(void);                // input bytes from the live session (after td_poll)
int td_getc(void);                 // the next one, or -1
int td_peek(void);                 // the next one without taking it, or -1
void td_putc(int c);               // output for the live session, kept until td_flush
void td_flush(void);
int td_live(void);                 // 1 while a session is logged in
int td_size(void);                 // its window as width | height << 16, 0 if the client did not say
const char *td_live_ip(void);      // its address, "" if none

#ifdef __cplusplus
}
#endif
#endif
