/* Prospero AutoDrive - prospero-smbd: service channel on TCP 4445
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * ASCII text lines, one request and one reply per line, fields separated
 * by single spaces; the number after the command is the protocol version.
 * The PC opens the connection and keeps it (docs/entwurf-phase2.md):
 *
 *   HELLO 1 [more fields]
 *       -> HELLO 1 server_id=<id> name=<name> share=<share>
 *          smb_port=<port> pairing=<open|closed> nonce=<16 hex>
 *   PAIR 1 pc=<name>
 *       -> PAIRED 1 user=<user> password=<password>
 *          or PAIR-REFUSED 1 reason=<closed|bad-name|storage>
 *   PING 1 [seq=<n>] utc_ms=<PC UTC, ms> [mac=<hex>]
 *       -> PONG 1 utc_ms=<server UTC, ms> skew_ms=<server - PC> trusted=<0|1>
 *   OPEN-PAIRING 1 seq=<n> mac=<hex>
 *       -> PAIRING-OPEN 1 seconds=<n>, or ERROR 1 reason=bad-mac
 *   STATUS 1
 *       -> STATUS 1, then "key=value" lines, then END 1
 *   RESET-STATS 1   restarts the counters of STATUS (measurements, P2.4)
 *       -> RESET-STATS 1
 *   TUNE 1 seq=<n> sndbuf=<bytes> credits=<n> mac=<hex>
 *       sets the send buffer and the credit limit, 0 = default, until the
 *       server ends (measurements, P2.4)
 *       -> TUNED 1 sndbuf=<bytes> credits=<n>, or ERROR 1 reason=bad-mac
 *          or reason=bad-value (sndbuf up to 16 MiB, credits up to 8192)
 *   QUIT 1          only from 127.0.0.1, i.e. a new instance on the console
 *       -> BYE 1, then the server ends
 *   anything else   -> ERROR 1 reason=<word>
 *
 * mac= is the last field: lowercase hex of HMAC-SHA256, keyed with the
 * password, over "<nonce of this connection> <line up to the space before
 * mac=>". So only a paired PC can make one, and a recorded line does not
 * work on another connection. On one connection seq must grow, so a line
 * cannot be replayed there either. A counter and not the time, because
 * the PC clock may be set back. Only a PING with a valid mac
 * counts for the clock (decision "variant 1"); a PING without one still
 * gets an answer, as a liveness check. A connection that sends nothing for
 * IDLE_SECONDS is closed.
 *
 * For curl.exe in the test scripts, an HTTP request is understood as well:
 * GET / answers the status, GET /quit (127.0.0.1 only) quits. The spike
 * payloads quit an earlier instance with GET /quit, and this server quits
 * them the same way, so all of them replace each other.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "config.h"
#include "hmac.h"
#include "service.h"

#define MAX_CLIENTS     8
#define LINE_SIZE       512
#define IDLE_SECONDS    60

typedef struct client {
  int fd;
  uint32_t peer;                /* IPv4, host byte order */
  time_t last;
  int http;                     /* 0, or the HTTP request seen: 'S' or 'Q' */
  char nonce[17];               /* sent with HELLO, keys every mac= */
  int64_t last_seq;             /* of the last line with a valid mac */
  size_t len;
  char buf[LINE_SIZE];
} client_t;

static struct {
  int fd;
  const service_info_t *info;
  client_t c[MAX_CLIENTS];
} g = {.fd = -1};


static int
send_all(int fd, const char *buf, size_t len) {
  while(len) {
    ssize_t n = send(fd, buf, len, 0);
    if(n <= 0) {
      return -1;
    }
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}


static void
drop(client_t *c) {
  close(c->fd);
  c->fd = -1;
}


/* The command word followed by " 1" and then the end or a space. */
static int
is_cmd(const char *line, const char *cmd) {
  size_t n = strlen(cmd);

  return !strncmp(line, cmd, n) && line[n] == ' ' && line[n + 1] == '1' &&
         (line[n + 2] == '\0' || line[n + 2] == ' ');
}


static int
status_text(char *buf, size_t size) {
  int n = g.info->status(buf, size);

  return n < 0 ? 0 : ((size_t)n >= size ? (int)size - 1 : n);
}


static int64_t
now_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}


/* Value of " key=" in line, up to the next space; its length or -1. */
static int
field(const char *line, const char *key, const char **val) {
  size_t klen = strlen(key);
  const char *p = line;

  while((p=strchr(p, ' '))) {
    p++;
    if(!strncmp(p, key, klen) && p[klen] == '=') {
      *val = p + klen + 1;
      return (int)strcspn(*val, " ");
    }
  }
  return -1;
}


/* A numeric field; -1 if missing or not digits. */
static int64_t
num_field(const char *line, const char *key) {
  const char *v;
  int64_t ms = 0;
  int n = field(line, key, &v);

  if(n < 1 || n > 18) {
    return -1;
  }
  for(int i=0; i<n; i++) {
    if(v[i] < '0' || v[i] > '9') {
      return -1;
    }
    ms = ms * 10 + (v[i] - '0');
  }
  return ms;
}


/* 1 if the line ends in a valid mac= for this connection and its seq is
 * higher than in the last line that had one. */
static int
mac_ok(client_t *c, const char *line) {
  const char *sp = strrchr(line, ' ');
  char msg[LINE_SIZE + 32];
  uint8_t mac[32];
  int64_t seq = num_field(line, "seq");
  int n;

  if(!c->nonce[0] || !sp || strncmp(sp + 1, "mac=", 4) || seq <= c->last_seq) {
    return 0;
  }
  n = snprintf(msg, sizeof msg, "%s %.*s", c->nonce, (int)(sp - line), line);
  if(n < 0 || n >= (int)sizeof msg) {
    return 0;
  }
  hmac_sha256(g.info->password, strlen(g.info->password), msg, (size_t)n, mac);
  if(!hmac_hex_equal(mac, sp + 5, strlen(sp + 5))) {
    return 0;
  }
  c->last_seq = seq;
  return 1;
}


static void
make_nonce(char out[17]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t raw[8];

  arc4random_buf(raw, sizeof raw);
  for(int i=0; i<8; i++) {
    out[2*i] = hex[raw[i] >> 4];
    out[2*i+1] = hex[raw[i] & 15];
  }
  out[16] = '\0';
}


static int
pair(client_t *c, const char *line, char *out, size_t size) {
  const char *v;
  char pc[CONFIG_PC_NAME_SIZE];
  int n = field(line, "pc", &v);

  if(n < 1 || n >= (int)sizeof pc || (int)strspn(v, CONFIG_PC_NAME_CHARS) < n) {
    return snprintf(out, size, "PAIR-REFUSED 1 reason=bad-name\n");
  }
  memcpy(pc, v, (size_t)n);
  pc[n] = '\0';
  if(!g.info->pairing_is_open()) {
    return snprintf(out, size, "PAIR-REFUSED 1 reason=closed\n");
  }
  if(g.info->paired(pc, c->peer) != 0) {
    return snprintf(out, size, "PAIR-REFUSED 1 reason=storage\n");
  }
  return snprintf(out, size, "PAIRED 1 user=%s password=%s\n",
                  g.info->user, g.info->password);
}


/* Answers the HTTP request once its header is complete, then closes. */
static int
answer_http(client_t *c) {
  char body[8192];
  char head[160];
  int quit = 0;
  int n;

  if(c->http == 'Q') {
    if(c->peer == INADDR_LOOPBACK) {
      quit = 1;
      n = snprintf(body, sizeof body, "PROSPERO-SMBD QUIT\n");
    } else {
      n = snprintf(body, sizeof body, "PROSPERO-SMBD QUIT REFUSED not-local\n");
    }
  } else {
    n = status_text(body, sizeof body);
  }
  snprintf(head, sizeof head,
           "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
           "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
  if(send_all(c->fd, head, strlen(head)) == 0) {
    send_all(c->fd, body, (size_t)n);
  }
  shutdown(c->fd, SHUT_WR);
  drop(c);
  return quit ? SERVICE_QUIT : SERVICE_OK;
}


/* Handles one complete line. Returns a service_result; the client may be
 * dropped afterwards. */
static int
handle_line(client_t *c, const char *line) {
  const service_info_t *in = g.info;
  char out[8192];
  int n;

  if(c->http) {
    return line[0] ? SERVICE_OK : answer_http(c);
  }
  if(!strncmp(line, "GET /", 5)) {
    c->http = (!strncmp(line + 5, "quit", 4) && (line[9] == ' ' || !line[9])) ? 'Q' : 'S';
    return SERVICE_OK;
  }

  if(is_cmd(line, "HELLO")) {
    if(!c->nonce[0]) {
      make_nonce(c->nonce);
    }
    n = snprintf(out, sizeof out,
                 "HELLO 1 server_id=%s name=%s share=%s smb_port=%u pairing=%s nonce=%s\n",
                 in->server_id, in->name, in->share, (unsigned)in->smb_port,
                 in->pairing_is_open() ? "open" : "closed", c->nonce);
  } else if(is_cmd(line, "PAIR")) {
    n = pair(c, line, out, sizeof out);
  } else if(is_cmd(line, "PING")) {
    int64_t pc = num_field(line, "utc_ms");
    int64_t now = now_ms();
    int trusted = pc >= 0 && mac_ok(c, line);

    if(pc < 0) {
      n = snprintf(out, sizeof out, "ERROR 1 reason=bad-time\n");
    } else {
      if(trusted) {
        in->clock(now - pc);
      }
      n = snprintf(out, sizeof out, "PONG 1 utc_ms=%lld skew_ms=%lld trusted=%d\n",
                   (long long)now, (long long)(now - pc), trusted);
    }
  } else if(is_cmd(line, "OPEN-PAIRING")) {
    if(mac_ok(c, line)) {
      in->open_pairing(c->peer);
      n = snprintf(out, sizeof out, "PAIRING-OPEN 1 seconds=%d\n", PAIRING_SECONDS);
    } else {
      n = snprintf(out, sizeof out, "ERROR 1 reason=bad-mac\n");
    }
  } else if(is_cmd(line, "TUNE")) {
    int64_t sb = num_field(line, "sndbuf");
    int64_t cr = num_field(line, "credits");

    if(sb < 0 || sb > (16 << 20) || cr < 0 || cr > 8192) {
      n = snprintf(out, sizeof out, "ERROR 1 reason=bad-value\n");
    } else if(!mac_ok(c, line)) {
      n = snprintf(out, sizeof out, "ERROR 1 reason=bad-mac\n");
    } else {
      in->tune((int)sb, (int)cr);
      n = snprintf(out, sizeof out, "TUNED 1 sndbuf=%d credits=%d\n", (int)sb, (int)cr);
    }
  } else if(is_cmd(line, "RESET-STATS")) {
    in->reset_stats();
    n = snprintf(out, sizeof out, "RESET-STATS 1\n");
  } else if(is_cmd(line, "STATUS")) {
    n = snprintf(out, sizeof out, "STATUS 1\n");
    n += status_text(out + n, sizeof out - n - 8);
    if(n > 0 && out[n - 1] != '\n') {
      out[n++] = '\n';
    }
    n += snprintf(out + n, sizeof out - n, "END 1\n");
  } else if(is_cmd(line, "QUIT")) {
    if(c->peer != INADDR_LOOPBACK) {
      n = snprintf(out, sizeof out, "ERROR 1 reason=not-local\n");
    } else {
      send_all(c->fd, "BYE 1\n", 6);
      shutdown(c->fd, SHUT_WR);
      drop(c);
      return SERVICE_QUIT;
    }
  } else {
    n = snprintf(out, sizeof out, "ERROR 1 reason=unknown-command\n");
  }
  if(send_all(c->fd, out, (size_t)n) != 0) {
    drop(c);
  }
  return SERVICE_OK;
}


static int
serve_client(client_t *c) {
  ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
  int result = SERVICE_OK;
  char *eol;

  if(n <= 0) {
    drop(c);
    return SERVICE_OK;
  }
  c->len += (size_t)n;
  c->last = time(NULL);

  while(c->fd >= 0 && result == SERVICE_OK &&
        (eol=memchr(c->buf, '\n', c->len))) {
    size_t used = (size_t)(eol - c->buf) + 1;

    *eol = '\0';
    if(eol > c->buf && eol[-1] == '\r') {
      eol[-1] = '\0';
    }
    result = handle_line(c, c->buf);
    memmove(c->buf, c->buf + used, c->len - used);
    c->len -= used;
  }
  if(c->fd >= 0 && c->len == sizeof c->buf) {
    send_all(c->fd, "ERROR 1 reason=line-too-long\n", 29);
    drop(c);
  }
  return result;
}


static void
accept_client(void) {
  struct sockaddr_in peer;
  socklen_t plen = sizeof peer;
  struct timeval tv = {1, 0};
  int fd;

  if((fd=accept(g.fd, (struct sockaddr*)&peer, &plen)) < 0) {
    return;
  }
  for(int i=0; i<MAX_CLIENTS; i++) {
    client_t *c = &g.c[i];
    if(c->fd < 0) {
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      memset(c, 0, sizeof *c);
      c->fd = fd;
      c->peer = peer.sin_family == AF_INET ? ntohl(peer.sin_addr.s_addr) : 0;
      c->last = time(NULL);
      return;
    }
  }
  send_all(fd, "ERROR 1 reason=busy\n", 20);
  close(fd);
}


int
service_open(uint16_t port, const service_info_t *info) {
  struct sockaddr_in addr;
  int one = 1;
  int fd;

  g.info = info;
  for(int i=0; i<MAX_CLIENTS; i++) {
    g.c[i].fd = -1;
  }
  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -errno;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 ||
     listen(fd, MAX_CLIENTS) != 0 ||
     fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
    int err = errno;
    close(fd);
    return -err;
  }
  g.fd = fd;
  return 0;
}


void
service_fdset(fd_set *rfds, int *maxfd) {
  if(g.fd < 0) {
    return;
  }
  FD_SET(g.fd, rfds);
  if(g.fd > *maxfd) {
    *maxfd = g.fd;
  }
  for(int i=0; i<MAX_CLIENTS; i++) {
    if(g.c[i].fd >= 0) {
      FD_SET(g.c[i].fd, rfds);
      if(g.c[i].fd > *maxfd) {
        *maxfd = g.c[i].fd;
      }
    }
  }
}


int
service_run(const fd_set *rfds) {
  time_t now = time(NULL);
  int result = SERVICE_OK;

  if(g.fd < 0) {
    return SERVICE_BROKEN;
  }
  for(int i=0; i<MAX_CLIENTS && result == SERVICE_OK; i++) {
    client_t *c = &g.c[i];
    if(c->fd < 0) {
      continue;
    }
    if(FD_ISSET(c->fd, rfds)) {
      result = serve_client(c);
    } else if(now - c->last > IDLE_SECONDS) {
      drop(c);
    }
  }
  if(result == SERVICE_OK && FD_ISSET(g.fd, rfds)) {
    int err;
    socklen_t elen = sizeof err;

    /* A pending error on the listener (e.g. after rest mode) means it
     * is gone; the caller reopens everything. */
    if(getsockopt(g.fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err) {
      return SERVICE_BROKEN;
    }
    accept_client();
  }
  return result;
}


void
service_close(void) {
  for(int i=0; i<MAX_CLIENTS; i++) {
    if(g.c[i].fd >= 0) {
      drop(&g.c[i]);
    }
  }
  if(g.fd >= 0) {
    close(g.fd);
    g.fd = -1;
  }
}


int
service_stop_previous(uint16_t port) {
  static const char req[] = "GET /quit HTTP/1.0\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval tv = {3, 0};
  char buf[256];
  int answered = 0;
  int fd;

  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return 0;
  }
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(connect(fd, (struct sockaddr*)&addr, sizeof addr) == 0 &&
     send_all(fd, req, sizeof req - 1) == 0) {
    answered = recv(fd, buf, sizeof buf, 0) > 0;
  }
  close(fd);
  return answered;
}
