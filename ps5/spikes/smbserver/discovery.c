/* Prospero AutoDrive - Spike S7: discovery and liveness over UDP
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * One text line per datagram, ASCII, fields separated by single spaces:
 *
 *   request: PROSPERO-DISCOVER 1 nonce=<1-32 hex> utc_ms=<PC UTC, ms>
 *   reply:   PROSPERO-HERE 1 nonce=<same> id=<16 hex> smb_port=<n>
 *              utc_ms=<server UTC, ms> skew_ms=<server - PC> name=<rest>
 *
 * The client sends the request by broadcast or unicast and the server
 * answers the sender directly. skew_ms is how far the server's clock runs
 * ahead of the PC's, plus the one-way delay (decision "variant 1"). The id
 * tells servers apart; in this spike it is random per start.
 *
 * Only calls already proven on the console are used: no sscanf, no
 * inet_ntop.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "discovery.h"

#define REQ_MAGIC   "PROSPERO-DISCOVER 1 "
#define MAX_NONCE   32

static struct {
  char id[17];
  unsigned long long requests;
  unsigned long long replies;
  unsigned long long invalid;
  char last_peer[24];
  long long last_skew_ms;
  long long min_skew_ms;
  long long max_skew_ms;
} d;


static int64_t
now_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}


static void
ensure_id(void) {
  static const char hex[] = "0123456789abcdef";
  uint8_t raw[8];

  if(d.id[0]) {
    return;
  }
  arc4random_buf(raw, sizeof raw);
  for(int i=0; i<8; i++) {
    d.id[2*i] = hex[raw[i] >> 4];
    d.id[2*i+1] = hex[raw[i] & 15];
  }
  d.id[16] = '\0';
}


/* Matches "<key>=" at *p and returns the value's length up to the next
 * space or the end; *p then points to the value. -1 if the key is not
 * there. */
static int
field(const char **p, const char *end, const char *key) {
  size_t klen = strlen(key);
  const char *v;

  if((size_t)(end - *p) <= klen || memcmp(*p, key, klen) != 0 || (*p)[klen] != '=') {
    return -1;
  }
  v = *p + klen + 1;
  *p = v;
  while(v < end && *v != ' ') {
    v++;
  }
  return (int)(v - *p);
}


int
discovery_reply(const char *req, size_t len, const char *name,
                uint16_t smb_port, int64_t now, char *out, size_t size) {
  const char *p = req;
  const char *end = req + len;
  const char *nonce;
  int64_t pc = 0;
  int nlen;
  int tlen;
  int n;

  while(end > p && (end[-1] == '\n' || end[-1] == '\r')) {
    end--;
  }
  if((size_t)(end - p) < sizeof REQ_MAGIC - 1 ||
     memcmp(p, REQ_MAGIC, sizeof REQ_MAGIC - 1) != 0) {
    return 0;
  }
  p += sizeof REQ_MAGIC - 1;

  if((nlen=field(&p, end, "nonce")) < 1 || nlen > MAX_NONCE) {
    return 0;
  }
  nonce = p;
  for(int i=0; i<nlen; i++) {
    char c = nonce[i];
    if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return 0;
    }
  }
  p += nlen;
  if(p >= end || *p++ != ' ') {
    return 0;
  }

  if((tlen=field(&p, end, "utc_ms")) < 1 || tlen > 18 || p + tlen != end) {
    return 0;
  }
  for(int i=0; i<tlen; i++) {
    if(p[i] < '0' || p[i] > '9') {
      return 0;
    }
    pc = pc * 10 + (p[i] - '0');
  }

  ensure_id();
  d.last_skew_ms = (long long)(now - pc);
  if(!d.replies || d.last_skew_ms < d.min_skew_ms) {
    d.min_skew_ms = d.last_skew_ms;
  }
  if(!d.replies || d.last_skew_ms > d.max_skew_ms) {
    d.max_skew_ms = d.last_skew_ms;
  }
  n = snprintf(out, size,
               "PROSPERO-HERE 1 nonce=%.*s id=%s smb_port=%u utc_ms=%lld "
               "skew_ms=%lld name=%s\n",
               nlen, nonce, d.id, (unsigned)smb_port, (long long)now,
               d.last_skew_ms, name);
  return (n < 0 || (size_t)n >= size) ? 0 : n;
}


int
discovery_open(uint16_t port) {
  struct sockaddr_in addr;
  int one = 1;
  int fd;

  if((fd=socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    return -errno;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
    int err = errno;
    close(fd);
    return -err;
  }
  ensure_id();
  return fd;
}


void
discovery_serve(int fd, const char *name, uint16_t smb_port) {
  struct sockaddr_in peer;
  socklen_t plen = sizeof peer;
  char req[256];
  char rep[512];
  ssize_t len;
  int n;

  len = recvfrom(fd, req, sizeof req, 0, (struct sockaddr*)&peer, &plen);
  if(len <= 0 || peer.sin_family != AF_INET) {
    return;
  }
  d.requests++;
  if(!(n=discovery_reply(req, (size_t)len, name, smb_port, now_ms(),
                         rep, sizeof rep))) {
    d.invalid++;
    return;
  }
  {
    const uint8_t *a = (const uint8_t*)&peer.sin_addr.s_addr;
    snprintf(d.last_peer, sizeof d.last_peer, "%u.%u.%u.%u:%u",
             a[0], a[1], a[2], a[3], (unsigned)ntohs(peer.sin_port));
  }
  if(sendto(fd, rep, (size_t)n, 0, (struct sockaddr*)&peer, plen) == n) {
    d.replies++;
  }
}


int
discovery_status(char *buf, size_t size) {
  int n = snprintf(buf, size,
                   "discovery_id=%s\ndiscovery_requests=%llu\n"
                   "discovery_replies=%llu\ndiscovery_invalid=%llu\n"
                   "discovery_last_peer=%s\ndiscovery_skew_ms=%lld (min %lld, max %lld)\n",
                   d.id, d.requests, d.replies, d.invalid,
                   d.last_peer[0] ? d.last_peer : "-",
                   d.last_skew_ms, d.min_skew_ms, d.max_skew_ms);
  return (n < 0 || (size_t)n >= size) ? (int)(size ? size - 1 : 0) : n;
}
