/* Prospero AutoDrive - prospero-smbd: SMB server for the PS5
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Shares the console's root "/" read-write as \\<ps5>\prospero over SMB on port
 * 445, with NTLMv2 and signing, through the libsmb2 server engine and our
 * own POSIX backend (backend.c). Runs until it is told to quit.
 *
 * State lives in /data/prospero-smbd/: config.ini (server ID, user,
 * password, paired PCs; created on the first start) and the ring log
 * log.txt. The agent on the PC finds the server over UDP 4445
 * (discovery.c), pairs once over the service channel on TCP 4445
 * (service.c) and then sends PING there as liveness and clock reference.
 *
 * Pairing (variant C, docs/design-phase2.md): open while no PC is paired;
 * afterwards only for PAIRING_SECONDS after a paired PC asked for it, and
 * for PAIRING_ON_START_S after each start, until one PC pairs: a PC that
 * lost its pairing (reinstalled) gets back in when the user restarts the
 * server (decision of 25.10.2026).
 *
 * One instance: a new start first asks a running one, or a spike payload,
 * to quit and waits until the ports are free. If the server loop fails, for
 * instance because rest mode took the sockets, everything is reopened after
 * a short pause (outer loop).
 *
 * Clock (P2.3): a PING with a valid mac measures how far the console
 * clock runs ahead of the PC. Rounded to whole seconds, it is applied to
 * every time stamp (backend_set_clock_skew) and kept in config.ini, so it
 * holds from the start until the next measurement. A new value is only
 * taken when it differs by CLOCK_HYSTERESIS_S or more, so times do not
 * jump by a second back and forth. Timers and durations (pairing windows,
 * idle timeout, uptime) run on the monotonic clock instead (monotime.h),
 * so setting the console's clock does not move them.
 *
 * Host build for the CI tests:
 *   prospero-smbd <share root> <state dir> <smb port>
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/select.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "backend.h"
#include "config.h"
#include "diag.h"
#include "discovery.h"
#include "log.h"
#include "monotime.h"
#include "service.h"
#include "signing.h"

#define NAME        "Prospero AutoDrive"
#define SHARE_NAME  "prospero"
#define PROFILE     'b'           /* 1 MiB socket buffers, TCP_NODELAY (S2b) */
#define MAX_CONNS   8
#define CLOCK_HYSTERESIS_S 2
#define PAIRING_ON_START_S 120

#ifndef PROSPERO_VERSION
#define PROSPERO_VERSION "0.0.0"
#endif


#ifdef __SCE__
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
#endif


static struct smb2_server g_server;
static config_t g_cfg;
static service_info_t g_info;
static const char *g_root;
static time_t g_start;                  /* mono_s() */
static unsigned g_connections;
static unsigned g_restarts;
static int g_quit;
static int g_announced;
static const char *g_state_dir;
static int g_udp_fd = -1;
static discovery_info_t g_disc;
static time_t g_pairing_until;          /* mono_s(), 0 = closed */
static int64_t g_skew_ms;
static unsigned g_skew_samples;
static char g_instance[17];          /* random per start, for test scripts */


/* PS5 notification and log line. Notifications vanish and do not stay in
 * the console's list, so they never carry anything the user must keep. */
static void
notify(const char *fmt, ...) {
  char msg[1024];
  va_list args;

  va_start(args, fmt);
  vsnprintf(msg, sizeof msg, fmt, args);
  va_end(args);

#ifdef __SCE__
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
#endif
  log_msg("%s", msg);
}


static void
ip_text(uint32_t ip, char out[48]) {
  snprintf(out, 48, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255),
           (unsigned)(ip >> 8 & 255), (unsigned)(ip & 255));
}


static int
pairing_is_open(void) {
  return g_cfg.npaired == 0 || mono_s() < g_pairing_until;
}


/* Stored before the password goes out, so a PC that got it is always on
 * the list. */
static int
on_paired(const char *pc, uint32_t peer) {
  config_t next = g_cfg;
  char ip[48];
  int rc;

  config_add_paired(&next, pc);
  if((rc=config_save(g_state_dir, &next)) != 0) {
    log_msg("pairing %s failed, config.ini not written: %s", pc, strerror(-rc));
    return rc;
  }
  g_cfg = next;
  g_pairing_until = 0;
  ip_text(peer, ip);
  notify(NAME ": paired with PC %s (%s)", pc, ip);
  return 0;
}


static void
on_open_pairing(uint32_t peer) {
  char ip[48];

  g_pairing_until = mono_s() + PAIRING_SECONDS;
  ip_text(peer, ip);
  notify(NAME ": pairing open for %d minutes (requested by %s)",
         PAIRING_SECONDS / 60, ip);
}


static void
on_clock(int64_t skew_ms) {
  long long s = (long long)((skew_ms >= 0 ? skew_ms + 500 : skew_ms - 500) / 1000);
  long long diff = s - g_cfg.skew_s;

  g_skew_ms = skew_ms;
  g_skew_samples++;
  if(g_cfg.has_skew && diff < CLOCK_HYSTERESIS_S && diff > -CLOCK_HYSTERESIS_S) {
    return;
  }
  log_msg("clock: console runs %lld ms ahead of the PC, time stamps now offset by %lld s (was %lld s%s)",
          (long long)skew_ms, s, g_cfg.skew_s, g_cfg.has_skew ? "" : ", unmeasured");
  {
    config_t next = g_cfg;
    int rc;

    next.has_skew = 1;
    next.skew_s = s;
    if((rc=config_save(g_state_dir, &next)) != 0) {
      log_msg("clock: config.ini not written: %s", strerror(-rc));
    }
    /* Applied even if not stored: the measurement is right either way. */
    g_cfg.has_skew = 1;
    g_cfg.skew_s = s;
  }
  backend_set_clock_skew(s);
}


static int g_tune_sndbuf;
static int g_tune_credits;


static void
tune(int sndbuf, int credits) {
  g_tune_sndbuf = sndbuf;
  g_tune_credits = credits;
  backend_set_sndbuf(sndbuf);
  g_server.credit_limit = (uint16_t)credits;
  log_msg("tune: send buffer %d, credit limit %d (0 = default)", sndbuf, credits);
}


static void
reset_stats(void) {
  backend_reset_stats();
  diag_reset();
}


static int
status(char *buf, size_t size) {
  time_t now = time(NULL);
  char now_utc[32];
  struct tm tm;
  int n;

  memset(&tm, 0, sizeof tm);
  gmtime_r(&now, &tm);
  strftime(now_utc, sizeof now_utc, "%Y-%m-%dT%H:%M:%SZ", &tm);
  n = snprintf(buf, size,
               "PROSPERO-SMBD OK\nversion=%s\ninstance=%s\nserver_id=%s\nname=%s\n"
               "share=%s\nroot=%s\nuser=%s\nsmb_port=%u\nuptime_s=%ld\n"
               "connections=%u\nrestarts=%u\nnow_utc=%s\npaired_pcs=%d\n"
               "pairing=%s\nskew_ms=%lld\nskew_samples=%u\nclock_skew_s=%lld%s\n",
               PROSPERO_VERSION, g_instance, g_cfg.server_id, g_cfg.name, SHARE_NAME,
               g_root, g_cfg.user, (unsigned)g_server.port,
               (long)(mono_s() - g_start), g_connections, g_restarts, now_utc,
               g_cfg.npaired, pairing_is_open() ? "open" : "closed",
               (long long)g_skew_ms, g_skew_samples, g_cfg.skew_s,
               g_cfg.has_skew ? "" : " (unmeasured)");
  if(n > 0 && (size_t)n < size) {
    n += discovery_status(buf + n, size - n);
  }
  if(n > 0 && (size_t)n < size) {
    n += snprintf(buf + n, size - n, "tune_sndbuf=%d\ntune_credits=%d\n",
                  g_tune_sndbuf, g_tune_credits);
  }
  if(n > 0 && (size_t)n < size) {
    n += signing_status(buf + n, size - n);
  }
  if(n > 0 && (size_t)n < size) {
    n += diag_status(buf + n, size - n);
  }
  if(n > 0 && (size_t)n < size) {
    n += backend_status(buf + n, size - n);
  }
  return n;
}


/* Hooks into smb2_serve_port(): it runs them at least every 100 ms. */
static void
extra_fdset(struct smb2_server *srv, fd_set *rfds, fd_set *wfds, int *maxfd) {
  (void)wfds;
  backend_listener(srv->fd);
  backend_fdset(rfds, maxfd);
  service_fdset(rfds, maxfd);
  if(g_udp_fd >= 0) {
    FD_SET(g_udp_fd, rfds);
    if(g_udp_fd > *maxfd) {
      *maxfd = g_udp_fd;
    }
  }
}


static void
extra_service(struct smb2_server *srv, fd_set *rfds, fd_set *wfds) {
  (void)wfds;
  if(!g_announced) {
    /* The SMB port is bound once the loop runs. */
    g_announced = 1;
    notify(NAME " running: share %s on SMB port %u", SHARE_NAME, (unsigned)srv->port);
  }
  backend_flush();
  diag_sample();
  if(g_udp_fd >= 0 && FD_ISSET(g_udp_fd, rfds)) {
    discovery_serve(g_udp_fd, &g_disc);
  }
  switch(service_run(rfds)) {
  case SERVICE_QUIT:
    {
      uint32_t peer = 0;
      const char *why = service_quit_reason(&peer);

      log_msg("quit requested on the service channel (%s, from %u.%u.%u.%u)", why ? why : "?",
              peer >> 24, (peer >> 16) & 255, (peer >> 8) & 255, peer & 255);
    }
    g_quit = 1;
    srv->stop_requested = 1;
    break;
  case SERVICE_BROKEN:
    log_msg("service channel listener failed");
    srv->stop_requested = 1;
    break;
  default:
    break;
  }
}


static void
on_new_client(struct smb2_context *smb2, void *cb_data) {
  (void)cb_data;
  g_connections++;
  backend_attach(smb2);
}


/* Opens the service channel; a quitting instance may still hold the port
 * for a moment. */
static int
open_service(int tries) {
  int rc = -1;

  for(int i=0; i<tries; i++) {
    if((rc=service_open(SERVICE_PORT, &g_info)) == 0) {
      return 0;
    }
    sleep(1);
  }
  return rc;
}


int
main(int argc, char **argv) {
  const char *state_dir = "/data/prospero-smbd";
  uint16_t port = 445;
  unsigned pause_s = 1;
  int failing = 0;
  int cfg_rc;
  int rc;

#ifndef __SCE__
  if(argc != 4) {
    fprintf(stderr, "usage: %s <share root> <state dir> <smb port>\n", argv[0]);
    return 2;
  }
  g_root = argv[1];
  state_dir = argv[2];
  port = (uint16_t)atoi(argv[3]);
#else
  (void)argc;
  (void)argv;
  g_root = "/";
#endif
  g_state_dir = state_dir;

  signal(SIGPIPE, SIG_IGN);
  g_start = mono_s();
  {
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[8];

    arc4random_buf(raw, sizeof raw);
    for(int i=0; i<8; i++) {
      g_instance[2*i] = hex[raw[i] >> 4];
      g_instance[2*i+1] = hex[raw[i] & 15];
    }
  }

  /* Creates state_dir, so the log can follow. */
  cfg_rc = config_load(state_dir, &g_cfg);
  if((rc=log_open(state_dir)) != 0) {
    printf("no log file in %s: %s\n", state_dir, strerror(-rc));
  }
  if(cfg_rc < 0) {
    notify(NAME ": ERROR: cannot read or create the settings in %s: %s", state_dir, strerror(-cfg_rc));
    return 1;
  }
  log_msg("prospero-smbd %s starting: root %s, smb port %u, server_id %s",
          PROSPERO_VERSION, g_root, (unsigned)port, g_cfg.server_id);
  if(cfg_rc == CONFIG_CREATED) {
    log_msg("config.ini created (first start)");
  } else if(cfg_rc == CONFIG_REPLACED) {
    notify(NAME ": config.ini was damaged and has been created anew "
           "(the old one is kept as config.ini.bad); PCs must pair again");
  }

  signing_check();

  if(service_stop_previous(SERVICE_PORT)) {
    log_msg("asked the running instance to quit");
  }

  if((rc=backend_init(g_root, SHARE_NAME, g_cfg.user, g_cfg.password, 1, PROFILE))) {
    notify(NAME ": ERROR: cannot share %s: %s", g_root, strerror(-rc));
    return 1;
  }
  /* Until the first PING the last measured value holds. */
  backend_set_clock_skew(g_cfg.skew_s);
  if(g_cfg.has_skew) {
    log_msg("clock: time stamps offset by %lld s (from config.ini)", g_cfg.skew_s);
  }
  g_info.server_id = g_cfg.server_id;
  g_info.name = g_cfg.name;
  g_info.share = SHARE_NAME;
  g_info.user = g_cfg.user;
  g_info.password = g_cfg.password;
  g_info.smb_port = port;
  g_info.status = status;
  if(g_cfg.npaired) {
    g_pairing_until = mono_s() + PAIRING_ON_START_S;
    log_msg("pairing open for %d s after the start", PAIRING_ON_START_S);
  }
  g_info.pairing_is_open = pairing_is_open;
  g_info.paired = on_paired;
  g_info.open_pairing = on_open_pairing;
  g_info.clock = on_clock;
  g_info.reset_stats = reset_stats;
  g_info.tune = tune;
  g_disc.server_id = g_cfg.server_id;
  g_disc.name = g_cfg.name;
  g_disc.service_port = SERVICE_PORT;
  g_disc.smb_port = port;

  snprintf(g_server.hostname, sizeof g_server.hostname, "%s", g_cfg.name);
  g_server.port = port;
  g_server.signing_enabled = 1;
  g_server.allow_anonymous = 0;
  g_server.handlers = &backend_handlers;
  g_server.extra_fdset = extra_fdset;
  g_server.extra_service = extra_service;

  /* Outer loop: a failed round reopens both ports after a pause that grows
   * to 30 s; a round that ran for a minute counts as healthy again. Only
   * the first failure of a streak is a notification. */
  while(!g_quit) {
    time_t round = mono_s();

    if((rc=open_service(g_restarts ? 1 : 10)) != 0) {
      log_msg("service port %d not available: %s", SERVICE_PORT, strerror(-rc));
    } else {
      /* Without discovery the agent still finds a known address. */
      if((g_udp_fd=discovery_open(DISCOVERY_PORT)) < 0) {
        log_msg("UDP port %d not available: %s", DISCOVERY_PORT, strerror(-g_udp_fd));
      }
      g_server.stop_requested = 0;
      rc = smb2_serve_port(&g_server, MAX_CONNS, on_new_client, NULL);
      service_close();
      if(g_udp_fd >= 0) {
        close(g_udp_fd);
        g_udp_fd = -1;
      }
      if(g_quit) {
        break;
      }
      log_msg("server loop ended (%d), reopening", rc);
    }
    if(mono_s() - round >= 60) {
      failing = 0;
      pause_s = 1;
    }
    if(!failing++) {
      notify(NAME ": network error, retrying");
    }
    g_restarts++;
    sleep(pause_s);
    pause_s = pause_s * 2 > 30 ? 30 : pause_s * 2;
  }

  notify(NAME ": stopped (%u SMB connections served)", g_connections);
  log_close();
  return 0;
}
