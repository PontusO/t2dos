// demosrv.c - the reference background server. Copy this file to start a new one.
//
//   demosrv [name] [-p port]        run it in the background:  demosrv &
//
// It answers requests sent through two named pipes, /pipe/NAME (requests) and
// /pipe/NAME.out (replies); NAME defaults to "demosrv". From the shell:
//
//   demosrv &
//   echo status > /pipe/demosrv       one request is one message (it ends when the writer closes)
//   cat /pipe/demosrv.out             waits for the reply, prints it
//
// Requests: status, echo TEXT, quit; anything else is answered with an error line.
// With -p PORT it also answers every TCP connection on PORT with the status line.
//
// The pattern every background program follows, in the order of main() below:
//
//   1. Create your resources: your pipes (mkpipe), a listener (listen), and open
//      what you will read. Do it first; if something fails, say so and exit.
//   2. Loop forever: WAIT for work, HANDLE it, REPLY, and YIELD when there was
//      nothing to do.
//   3. Leave cleanly: finish what the client is still reading, then return.
//
// THE RULE: there is no preemption. A job runs until it calls something that waits,
// and only then can the shell, an editor or another job run. So a job must reach a
// waiting call regularly: read of an empty pipe, sleep, accept, recv, getchar, or
// yield(). A loop that never does freezes the whole board. Never poll in a tight
// loop without yield().
//
// Pipe facts used here:
//   - mkpipe("x") makes /pipe/x, owned by the program that made it. When the owner
//     exits or is killed, its pipes disappear at once, with any unread data.
//   - A message ends when its writer closes. read() returns the bytes, then 0 once
//     at the end of the message. read() on an empty pipe waits (a switch point).
//   - stat() on a pipe gives size = bytes queued and mtime = complete messages queued
//     (pipes have no time). Bytes alone are not enough: a writer may have sent half a
//     request and not closed yet. When mtime > 0, reading one message will not wait.
//   - A reply is written by opening the .out pipe, writing, and closing it: one
//     reply, one message. A client reading .out before the reply exists just waits.
#include "sys.h"

char inpath[48];
char outname[40];
char outpath[48];
char msg[1024];          // the request
char rep[1100];          // the reply
struct stat_t st;
int started;             // millis() at start
int requests;            // requests handled so far

// Write rep as one message to the reply pipe.
int reply(int len) {
  int fd; int w;
  fd = open(outpath, OPEN_WRITE);
  if (fd < 0) return -1;
  w = write(fd, rep, len);
  close(fd);
  return w;
}

// The status line, in rep, with a newline; returns its length.
int status_line() {
  return sprintf(rep, "demosrv: up %d s, %d requests\n", (millis() - started) / 1000, requests);
}

// Read one whole message from the request fd into msg, NUL terminated, trailing
// newlines and spaces dropped (the shell's echo adds a newline). Returns its length.
int read_request(int fd) {
  int len; int r;
  len = 0;
  r = read(fd, msg, 1023);
  while (r > 0) {
    len = len + r;
    r = read(fd, msg + len, 1023 - len);
  }
  while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r' || msg[len - 1] == ' ')) len = len - 1;
  msg[len] = 0;
  return len;
}

// Handle the request in msg. Returns 1 for quit, else 0.
int handle() {
  int i; int n;
  requests = requests + 1;
  i = 0;
  while (msg[i] && msg[i] != ' ') i = i + 1;     // i: end of the first word
  if (msg[i]) { msg[i] = 0; i = i + 1; }         // the rest, if any, starts at msg + i
  printf("demosrv: %s\n", msg);                  // the console log
  if (strcmp(msg, "status") == 0) {
    n = status_line();
    reply(n);
  } else if (strcmp(msg, "echo") == 0) {
    n = sprintf(rep, "%s\n", msg + i);
    reply(n);
  } else if (strcmp(msg, "quit") == 0) {
    reply(sprintf(rep, "bye\n"));
    return 1;
  } else {
    n = sprintf(rep, "error: unknown command '%s'\n", msg);
    reply(n);
  }
  return 0;
}

// Answer one TCP connection with the status line, then close it.
int serve_tcp(int sock) {
  int n;
  requests = requests + 1;
  printf("demosrv: tcp status\n");
  n = status_line();
  send(sock, rep, n);
  close(sock);
  return 0;
}

int main(int argc, char **argv) {
  char *name; int port; int i; int fd; int sock; int quit; int busy;
  name = "demosrv";
  port = 0;
  i = 1;
  while (i < argc) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) { port = atoi(argv[i + 1]); i = i + 2; }
    else { name = argv[i]; i = i + 1; }
  }
  if (strlen(name) > 27) { printf("demosrv: name too long\n"); return 2; }

  // 1. Create the resources. Pipe names are given without the /pipe/ prefix.
  sprintf(outname, "%s.out", name);
  sprintf(inpath, "/pipe/%s", name);
  sprintf(outpath, "/pipe/%s", outname);
  if (mkpipe(name)) { printf("demosrv: mkpipe %s failed\n", name); return 1; }
  if (mkpipe(outname)) { printf("demosrv: mkpipe %s failed\n", outname); return 1; }
  fd = open(inpath, OPEN_READ);
  if (fd < 0) { printf("demosrv: open %s failed\n", inpath); return 1; }
  if (port) {
    // listen() returns 0 even with the WiFi down, so ask net_status() first.
    if (net_status() != NET_UP) { printf("demosrv: network is down\n"); port = 0; }
    else if (listen(port)) { printf("demosrv: cannot listen on %d\n", port); port = 0; }
  }
  started = millis();
  requests = 0;
  quit = 0;

  // 2. The main loop: wait, handle, reply, yield.
  while (!quit) {
    if (!port) {
      // Pipe only: the read waits until a request comes. Waiting is what lets the
      // rest of the system run, so nothing else is needed here.
      read_request(fd);
      quit = handle();
    } else {
      // With a listener there are two things to wait for and one blocking call
      // cannot wait for both. So check each without waiting, and yield when
      // neither had anything.
      busy = 0;
      sock = accept(0);                          // 0: do not wait
      if (sock >= 0) { serve_tcp(sock); busy = 1; }
      if (stat(inpath, &st) == 0 && st.mtime > 0) {  // a whole request is queued: read will not block
        read_request(fd);
        quit = handle();
        busy = 1;
      }
      if (!busy) yield();                        // idle: let the shell and other jobs run
    }
  }

  // 3. Leave cleanly. The kernel removes a program's pipes the moment it exits, so
  // an unread reply would vanish with them. Wait until the client has read the
  // reply (the .out pipe is empty), but only for a while: a client that never
  // reads must not keep us alive. Waiting calls only, as always.
  i = 0;
  while (i < 500 && stat(outpath, &st) == 0 && st.size > 0) { sleep(10); i = i + 1; }
  return 0;
}
