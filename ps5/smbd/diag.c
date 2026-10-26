/* Prospero AutoDrive - prospero-smbd: queue diagnosis (P2.4)
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Where does a reply wait under load (S4: a small file took 2.9 s during a
 * big copy instead of 44 ms)? Sampled once per loop pass, per connection:
 *
 *   outq   replies in libsmb2's send queue and their unsent bytes: they
 *          leave strictly in order, so a small reply waits behind all of
 *          them
 *   waitq  requests libsmb2 still waits on a reply for
 *   sock   bytes in the socket's send buffer the peer has not taken yet
 *          (FIONWRITE on the console, SIOCOUTQ on Linux; -1 if unknown)
 *   gap    time between two loop passes: long ones mean the server itself
 *          is busy
 *   credits  credits the clients hold plus the charge of their requests in
 *          flight (patch libsmb2-credit-limit), one per 64 KiB of READ or
 *          WRITE: how much work they may have outstanding
 *
 * The send queue is a private field of libsmb2 (pinned commit), read only.
 * Only measured here, nothing changes behaviour.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef __linux__
#include <linux/sockios.h>
#else
#include <sys/filio.h>
#endif

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include "libsmb2-private.h"

#include "diag.h"
#include "monotime.h"

typedef struct {
  long long cur;
  long long max;
  long long sum;
} gauge_t;

static struct {
  unsigned long long samples;
  gauge_t outq_pdus;
  gauge_t outq_bytes;
  gauge_t waitq_pdus;
  gauge_t sock_bytes;
  gauge_t gap_ms;
  gauge_t credits;
  int sock_known;
  int sndbuf;
  int64_t last_ms;
} d = {.sock_known = 1};


static void
put(gauge_t *g, long long v) {
  g->cur = v;
  g->sum += v;
  if(v > g->max) {
    g->max = v;
  }
}


void
diag_sample(void) {
  long long opdus = 0, obytes = 0, wpdus = 0, sock = 0, credits = 0;
  int64_t now = mono_ms();

  for(struct smb2_context *c = smb2_active_contexts(); c; c = c->next) {
    int fd = smb2_get_fd(c);

    if(fd < 0) {
      continue;
    }
    for(struct smb2_pdu *p = c->outqueue; p; p = p->next) {
      opdus++;
      obytes += (long long)(p->out.total_size - p->out.num_done);
    }
    for(struct smb2_pdu *p = c->waitqueue; p; p = p->next) {
      wpdus++;
    }
    credits += 1 + c->srv_credits_net;
    if(d.sock_known) {
      int n = 0;
#ifdef __linux__
      int rc = ioctl(fd, SIOCOUTQ, &n);
#else
      int rc = ioctl(fd, FIONWRITE, &n);
#endif
      if(rc == 0) {
        sock += n;
      } else {
        d.sock_known = 0;
      }
    }
    if(!d.sndbuf) {
      socklen_t len = sizeof d.sndbuf;
      getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &d.sndbuf, &len);
    }
  }
  d.samples++;
  put(&d.outq_pdus, opdus);
  put(&d.outq_bytes, obytes);
  put(&d.waitq_pdus, wpdus);
  put(&d.sock_bytes, d.sock_known ? sock : -1);
  put(&d.gap_ms, d.last_ms ? (long long)(now - d.last_ms) : 0);
  put(&d.credits, credits);
  d.last_ms = now;
}


void
diag_reset(void) {
  int known = d.sock_known;

  /* The send buffer is read again: it may have been changed. */
  memset(&d, 0, sizeof d);
  d.sock_known = known;
}


static int
gauge(char *buf, size_t size, const char *name, const gauge_t *g) {
  long long avg = d.samples ? g->sum / (long long)d.samples : 0;

  return snprintf(buf, size, "diag_%s=%lld (max %lld, avg %lld)\n", name, g->cur, g->max, avg);
}


int
diag_status(char *buf, size_t size) {
  const gauge_t *gs[] = {&d.outq_pdus, &d.outq_bytes, &d.waitq_pdus, &d.sock_bytes, &d.gap_ms,
                         &d.credits};
  const char *names[] = {"outq_pdus", "outq_bytes", "waitq_pdus", "sock_unsent_bytes", "loop_gap_ms",
                         "credits"};
  int n = snprintf(buf, size, "diag_samples=%llu\ndiag_sndbuf=%d\n", d.samples, d.sndbuf);

  for(size_t i=0; i<sizeof gs / sizeof gs[0] && n > 0 && (size_t)n < size; i++) {
    n += gauge(buf + n, size - n, names[i], gs[i]);
  }
  return (n < 0 || (size_t)n >= size) ? (int)(size ? size - 1 : 0) : n;
}
