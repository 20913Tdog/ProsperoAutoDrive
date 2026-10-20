/* Prospero AutoDrive - Spike S2: read-only SMB server prototype
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Question answered by this spike: does the libsmb2 server engine with our
 * own file system backend serve a folder to the Windows 11 SMB client
 * (NTLMv2, signing required) correctly and completely?
 *
 * The payload creates a new test folder with deterministic files
 * (testdata.c) and shares exactly that folder, read-only, as \\<ps5>\s2.
 * User "prospero", random password shown as a PS5 notification. GET / on
 * TCP 4445 reports status and the requests the backend does not support,
 * GET /quit stops the server. It also stops by itself after RUN_SECONDS.
 * For the speed diagnosis (S2b), GET /profile/<a|b|c> switches socket and
 * dialect settings and GET /reset restarts the timing counters.
 *
 * Host build for the CI tests: s2-smbserver <base dir> <smb port> <password>
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "fsro.h"
#include "testdata.h"

#define RUN_SECONDS  900
#define CONTROL_PORT 4445
#define SHARE_NAME   "s2"
#define USER_NAME    "prospero"


#ifdef __SCE__
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
#endif


static struct smb2_server g_server;
static char g_folder[1024];
static time_t g_start;
static int g_ctl_fd = -1;
static unsigned g_connections;


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
  printf("%s\n", msg);
  fflush(stdout);
}


/* Ask an earlier S1/S2 instance (if any) to exit, so that it frees 445. */
static void
stop_previous_instance(void) {
  static const char req[] = "GET /quit HTTP/1.0\r\n\r\n";
  struct sockaddr_in addr;
  int fd;

  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return;
  }
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONTROL_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if(connect(fd, (struct sockaddr*)&addr, sizeof addr) == 0) {
    if(send(fd, req, sizeof req - 1, 0) > 0) {
      sleep(2);
    }
  }
  close(fd);
}


static int
open_control_port(void) {
  struct sockaddr_in addr;
  int one = 1;
  int fd;

  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONTROL_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 ||
     listen(fd, 8) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}


#ifdef __SCE__
/* Password for the notification: 3 groups of 4, no look-alike characters. */
static void
make_password(char *out, size_t size) {
  static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
  size_t n = 0;

  for(int i=0; i<12 && n+2 < size; i++) {
    if(i && i % 4 == 0) {
      out[n++] = '-';
    }
    out[n++] = alphabet[arc4random_uniform(sizeof alphabet - 1)];
  }
  out[n] = '\0';
}
#endif


/* Serve one HTTP request on the control port. */
static void
serve_control(int fd) {
  struct timeval tv = {2, 0};
  char req[1024];
  char body[8192];
  char head[128];
  ssize_t len;
  int n;

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  len = recv(fd, req, sizeof req - 1, 0);
  req[len > 0 ? len : 0] = '\0';

  if(strncmp(req, "GET /quit", 9) == 0) {
    g_server.stop_requested = 1;
    n = snprintf(body, sizeof body, "PROSPERO-S2 QUIT\n");
  } else if(strncmp(req, "GET /reset", 10) == 0) {
    fsro_reset_stats();
    n = snprintf(body, sizeof body, "PROSPERO-S2 RESET\n");
  } else if(strncmp(req, "GET /profile/", 13) == 0) {
    n = snprintf(body, sizeof body, "PROSPERO-S2 %s\n",
                 fsro_set_profile(req[13]) == 0 ? "PROFILE" : "UNKNOWN PROFILE");
  } else {
    long up = (long)(time(NULL) - g_start);
    n = snprintf(body, sizeof body,
                 "PROSPERO-S2 OK\nfolder=%s\nshare=%s\nuser=%s\n"
                 "uptime_s=%ld\nremaining_s=%ld\nconnections=%u\n",
                 g_folder, SHARE_NAME, USER_NAME, up,
                 (long)RUN_SECONDS - up, g_connections);
    if(n > 0 && (size_t)n < sizeof body) {
      n += fsro_status(body + n, sizeof body - n);
    }
  }
  if(n < 0) {
    n = 0;
  } else if((size_t)n >= sizeof body) {
    n = sizeof body - 1;
  }

  snprintf(head, sizeof head,
           "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
           "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
  send(fd, head, strlen(head), 0);
  send(fd, body, n, 0);
  close(fd);
}


/* Hooks into smb2_serve_port(): it runs them at least every 100 ms. */
static void
extra_fdset(struct smb2_server *srv, fd_set *rfds, fd_set *wfds, int *maxfd) {
  (void)wfds;
  fsro_listener(srv->fd);
  FD_SET(g_ctl_fd, rfds);
  if(g_ctl_fd > *maxfd) {
    *maxfd = g_ctl_fd;
  }
}


static void
extra_service(struct smb2_server *srv, fd_set *rfds, fd_set *wfds) {
  (void)wfds;
  fsro_flush();
  if(FD_ISSET(g_ctl_fd, rfds)) {
    int fd = accept(g_ctl_fd, NULL, NULL);
    if(fd >= 0) {
      serve_control(fd);
    }
  }
  if(time(NULL) - g_start >= RUN_SECONDS) {
    srv->stop_requested = 1;
  }
}


static void
on_new_client(struct smb2_context *smb2, void *cb_data) {
  (void)cb_data;
  g_connections++;
  fsro_attach(smb2);
}


int
main(int argc, char **argv) {
  const char *base = "/data";
  char password[32];
  uint16_t port = 445;
  int rc;

#ifndef __SCE__
  if(argc != 4) {
    fprintf(stderr, "usage: %s <base dir> <smb port> <password>\n", argv[0]);
    return 2;
  }
  base = argv[1];
  port = (uint16_t)atoi(argv[2]);
  snprintf(password, sizeof password, "%s", argv[3]);
#else
  (void)argc;
  (void)argv;
  make_password(password, sizeof password);
#endif

  signal(SIGPIPE, SIG_IGN);
  g_start = time(NULL);
  stop_previous_instance();

  if((g_ctl_fd=open_control_port()) < 0) {
    notify("Prospero S2: Port %d nicht frei: %s", CONTROL_PORT, strerror(errno));
    return 1;
  }

  notify("Prospero S2: lege Testordner an ...");
  if((rc=testdata_create(base, g_folder, sizeof g_folder))) {
    notify("Prospero S2: FEHLER Testordner: %s (%s)", strerror(-rc), g_folder);
    return 1;
  }
  if((rc=fsro_init(g_folder, SHARE_NAME, USER_NAME, password))) {
    notify("Prospero S2: FEHLER Freigabe: %s", strerror(-rc));
    return 1;
  }

  snprintf(g_server.hostname, sizeof g_server.hostname, "PROSPERO-S2");
  g_server.port = port;
  g_server.signing_enabled = 1;
  g_server.allow_anonymous = 0;
  g_server.handlers = &fsro_handlers;
  g_server.extra_fdset = extra_fdset;
  g_server.extra_service = extra_service;

  notify("Prospero S2 bereit: Freigabe %s, Benutzer %s, Passwort %s (%d min)",
         SHARE_NAME, USER_NAME, password, RUN_SECONDS / 60);
  rc = smb2_serve_port(&g_server, 8, on_new_client, NULL);
  if(rc < 0) {
    notify("Prospero S2: SMB-Port %u nicht verfuegbar (%d)", port, rc);
  } else {
    notify("Prospero S2: beendet (%u Verbindungen)", g_connections);
  }

  close(g_ctl_fd);
  return rc < 0 ? 1 : 0;
}
