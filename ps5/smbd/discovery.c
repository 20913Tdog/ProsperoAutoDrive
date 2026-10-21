/* Prospero AutoDrive - prospero-smbd: discovery over UDP
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * As in spike S7 (passed E3), one ASCII line per datagram:
 *
 *   request: PROSPERO-DISCOVER 1 nonce=<1-32 lowercase hex> [more fields]
 *   reply:   PROSPERO-HERE 1 nonce=<same> id=<server ID>
 *              service_port=<n> smb_port=<n> name=<rest of the line>
 *
 * The agent only needs this when the stored address does not answer on the
 * service channel: it then broadcasts the request on every network. More
 * fields after the nonce are ignored, so later agents can add some. The
 * time is no longer here: it comes with PING on the service channel.
 *
 * Only calls already proven on the console are used: no sscanf, no
 * inet_ntop.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "discovery.h"

#define REQ_MAGIC "PROSPERO-DISCOVER 1 nonce="
#define MAX_NONCE 32

static struct {
  unsigned long long requests;
  unsigned long long replies;
  unsigned long long invalid;
  char last_peer[24];
} d;


int
discovery_reply(const char *req, size_t len, const discovery_info_t *info,
                char *out, size_t size) {
  const char *end = req + len;
  const char *nonce;
  int nlen = 0;
  int n;

  while(end > req && (end[-1] == '\n' || end[-1] == '\r')) {
    end--;
  }
  if((size_t)(end - req) < sizeof REQ_MAGIC - 1 ||
     memcmp(req, REQ_MAGIC, sizeof REQ_MAGIC - 1) != 0) {
    return 0;
  }
  nonce = req + sizeof REQ_MAGIC - 1;
  while(nonce + nlen < end && nonce[nlen] != ' ') {
    char c = nonce[nlen++];
    if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return 0;
    }
  }
  if(nlen < 1 || nlen > MAX_NONCE) {
    return 0;
  }
  n = snprintf(out, size,
               "PROSPERO-HERE 1 nonce=%.*s id=%s service_port=%u smb_port=%u name=%s\n",
               nlen, nonce, info->server_id, (unsigned)info->service_port,
               (unsigned)info->smb_port, info->name);
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
  return fd;
}


void
discovery_serve(int fd, const discovery_info_t *info) {
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
  if(!(n=discovery_reply(req, (size_t)len, info, rep, sizeof rep))) {
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
                   "discovery_requests=%llu\ndiscovery_replies=%llu\n"
                   "discovery_invalid=%llu\ndiscovery_last_peer=%s\n",
                   d.requests, d.replies, d.invalid,
                   d.last_peer[0] ? d.last_peer : "-");
  return (n < 0 || (size_t)n >= size) ? (int)(size ? size - 1 : 0) : n;
}
