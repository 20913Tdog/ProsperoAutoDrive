/* Prospero AutoDrive - prospero-smbd: SMB server for the PS5
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Shares the console's root "/" read-write as \\<ps5>\PS5 over SMB on port
 * 445, with NTLMv2 and signing, through the libsmb2 server engine and our
 * own POSIX backend (backend.c). Runs until it is told to quit.
 *
 * State lives in /data/prospero-smbd/: config.ini (server ID, user,
 * password; created on the first start) and the ring log log.txt. The
 * service channel on TCP 4445 (service.c) reports the server to the PC.
 *
 * One instance: a new start first asks a running one, or a spike payload,
 * to quit and waits until the ports are free. If the server loop fails, for
 * instance because rest mode took the sockets, everything is reopened after
 * a short pause (outer loop).
 *
 * Phase 2, work package P2.1. Discovery, pairing and the clock offset
 * follow in P2.2 and P2.3; until then the password is only in config.ini.
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
#include "log.h"
#include "service.h"

#define NAME        "Prospero AutoDrive"
#define SHARE_NAME  "PS5"
#define PROFILE     'b'           /* 1 MiB socket buffers, TCP_NODELAY (S2b) */
#define MAX_CONNS   8

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
static time_t g_start;
static unsigned g_connections;
static unsigned g_restarts;
static int g_quit;
static int g_announced;


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
               "PROSPERO-SMBD OK\nversion=%s\nserver_id=%s\nname=%s\n"
               "share=%s\nroot=%s\nuser=%s\nsmb_port=%u\nuptime_s=%ld\n"
               "connections=%u\nrestarts=%u\nnow_utc=%s\n",
               PROSPERO_VERSION, g_cfg.server_id, g_cfg.name, SHARE_NAME,
               g_root, g_cfg.user, (unsigned)g_server.port,
               (long)(now - g_start), g_connections, g_restarts, now_utc);
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
  service_fdset(rfds, maxfd);
}


static void
extra_service(struct smb2_server *srv, fd_set *rfds, fd_set *wfds) {
  (void)wfds;
  if(!g_announced) {
    /* The SMB port is bound once the loop runs. */
    g_announced = 1;
    notify(NAME " laeuft: Freigabe %s auf Port %u", SHARE_NAME, (unsigned)srv->port);
  }
  backend_flush();
  switch(service_run(rfds)) {
  case SERVICE_QUIT:
    log_msg("quit requested on the service channel");
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

  signal(SIGPIPE, SIG_IGN);
  g_start = time(NULL);

  /* Creates state_dir, so the log can follow. */
  cfg_rc = config_load(state_dir, &g_cfg);
  if((rc=log_open(state_dir)) != 0) {
    printf("no log file in %s: %s\n", state_dir, strerror(-rc));
  }
  if(cfg_rc < 0) {
    notify(NAME ": FEHLER Konfiguration in %s: %s", state_dir, strerror(-cfg_rc));
    return 1;
  }
  log_msg("prospero-smbd %s starting: root %s, smb port %u, server_id %s",
          PROSPERO_VERSION, g_root, (unsigned)port, g_cfg.server_id);
  if(cfg_rc == CONFIG_CREATED) {
    log_msg("config.ini created (first start)");
  } else if(cfg_rc == CONFIG_REPLACED) {
    notify(NAME ": config.ini war fehlerhaft und wurde neu angelegt "
           "(die alte liegt als config.ini.bad daneben)");
  }

  if(service_stop_previous(SERVICE_PORT)) {
    log_msg("asked the running instance to quit");
  }

  if((rc=backend_init(g_root, SHARE_NAME, g_cfg.user, g_cfg.password, 1, PROFILE))) {
    notify(NAME ": FEHLER Freigabe %s: %s", g_root, strerror(-rc));
    return 1;
  }
  g_info.server_id = g_cfg.server_id;
  g_info.name = g_cfg.name;
  g_info.share = SHARE_NAME;
  g_info.smb_port = port;
  g_info.status = status;

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
    time_t round = time(NULL);

    if((rc=open_service(g_restarts ? 1 : 10)) != 0) {
      log_msg("service port %d not available: %s", SERVICE_PORT, strerror(-rc));
    } else {
      g_server.stop_requested = 0;
      rc = smb2_serve_port(&g_server, MAX_CONNS, on_new_client, NULL);
      service_close();
      if(g_quit) {
        break;
      }
      log_msg("server loop ended (%d), reopening", rc);
    }
    if(time(NULL) - round >= 60) {
      failing = 0;
      pause_s = 1;
    }
    if(!failing++) {
      notify(NAME ": Netzwerkfehler, versuche es weiter");
    }
    g_restarts++;
    sleep(pause_s);
    pause_s = pause_s * 2 > 30 ? 30 : pause_s * 2;
  }

  notify(NAME ": beendet (%u Verbindungen)", g_connections);
  log_close();
  return 0;
}
